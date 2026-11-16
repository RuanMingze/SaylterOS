/* ============================================================
 * slyzc_gen.c - 语义解析与字节码生成
 * ------------------------------------------------------------
 * 生成策略：
 *   1. 预扫描每个函数的局部变量并发峰值，得到固定帧槽数；
 *      函数入口压入对应数量的 nil 占位，帧基址之上就是槽数组。
 *   2. 顶层语句先编译，随后（若存在 main）追加 CALL main 与 HALT，
 *      因此全局变量的初始化一定先于 main。
 *   3. 函数入口地址在编译时确定；前向调用先写 0 占位，全部函数
 *      编译完再统一回填，天然支持递归与互相调用。
 *
 * 栈深度只用于估算运行栈上限并做溢出保护：编译期维护当前深度，
 * 取观测最大值加安全余量写入 stack_hint，虚拟机按此校验。
 * ============================================================ */

#include "slyzc.h"

/* ---------- 回填记录 ---------- */

typedef struct {
    uint32_t at;
} Patch;

typedef struct {
    Patch *v;
    int    n;
    int    cap;
} PatchVec;

static void pv_push(PatchVec *p, uint32_t at) {
    if (p->n >= p->cap) {
        int next = p->cap ? p->cap * 2 : 16;
        Patch *nv = (Patch *)xmalloc(sizeof(Patch) * (size_t)next);
        for (int i = 0; i < p->n; i++) nv[i] = p->v[i];
        if (p->v) free(p->v);
        p->v = nv;
        p->cap = next;
    }
    p->v[p->n].at = at;
    p->n++;
}

/* ---------- 变量绑定 ---------- */

typedef struct {
    char *name;
    int   slot;
} VarBind;

typedef struct LoopCtx {
    struct LoopCtx *prev;
    PatchVec        breaks;
    PatchVec        continues;
    int             is_for;
} LoopCtx;

typedef struct {
    Compiler  *cc;
    FnDef     *fn;
    VarBind   *vars;
    int        var_count;
    int        var_cap;
    int        depth;
    int        max_depth;
    int        terminated;
    int        in_top;
    LoopCtx   *loop;
} Gen;

/* ---------- 内置函数表 ----------
 * argc 为 -1 表示参数个数可变，由调用点自行校验。 */

typedef struct {
    const char *name;
    int         id;
    int         argc;
} BuiltinDef;

static const BuiltinDef g_builtins[] = {
    /* 输出 */
    { "print",          SLE_SYS_PRINT,       1 },
    { "println",        SLE_SYS_PRINTLN,     1 },
    { "eprint",         SLE_SYS_EPRINT,      1 },

    /* 文件系统 */
    { "fs.read",        SLE_SYS_FS_READ,     1 },
    { "fs.write",       SLE_SYS_FS_WRITE,    2 },
    { "fs.exists",      SLE_SYS_FS_EXISTS,   1 },
    { "fs.list",        SLE_SYS_FS_LIST,     1 },
    { "fs.remove",      SLE_SYS_FS_REMOVE,   1 },
    { "fs.mkdir",       SLE_SYS_FS_MKDIR,    1 },
    { "fs.entries",     SLE_SYS_FS_ENTRIES,  1 },

    /* 配置 */
    { "cfg.get",        SLE_SYS_CFG_GET,     1 },
    { "cfg.set",        SLE_SYS_CFG_SET,     2 },
    { "cfg.keys",       SLE_SYS_CFG_KEYS,    0 },

    /* 时间 */
    { "time.now",       SLE_SYS_TIME_NOW,    0 },
    { "time.ms",        SLE_SYS_TIME_MS,     0 },
    { "time.sleep",     SLE_SYS_SLEEP,       1 },
    { "time.text",      SLE_SYS_TIME_TEXT,   0 },
    { "time.date",      SLE_SYS_DATE_TEXT,   0 },

    /* 进程 */
    { "os.exec",        SLE_SYS_EXEC,        1 },
    { "os.exit",        SLE_SYS_EXIT,        1 },
    { "os.beep",        SLE_SYS_BEEP,        2 },
    { "os.open",        SLE_SYS_OPEN_UI,     1 },
    { "os.info",        SLE_SYS_INFO,        1 },
    { "os.proc",        SLE_SYS_PROC,        2 },
    { "os.proc_name",   SLE_SYS_PROC_NAME,   1 },

    /* 媒体与音频 */
    { "media.request",  SLE_SYS_MEDIA_REQ,   0 },
    { "audio.play",     SLE_SYS_AUDIO_PLAY,  1 },
    { "audio.stop",     SLE_SYS_AUDIO_STOP,  0 },
    { "audio.pause",    SLE_SYS_AUDIO_PAUSE, 1 },
    { "audio.state",    SLE_SYS_AUDIO_STATE, 0 },
    { "audio.position", SLE_SYS_AUDIO_POS,   0 },
    { "audio.duration", SLE_SYS_AUDIO_DUR,   0 },
    { "audio.set_volume", SLE_SYS_AUDIO_VOL, 1 },
    { "audio.volume",   SLE_SYS_AUDIO_GETVOL, 0 },

    /* 窗口 */
    { "win.open",       SLE_SYS_WIN_OPEN,    3 },
    { "win.close",      SLE_SYS_WIN_CLOSE,   1 },
    { "win.width",      SLE_SYS_WIN_SIZE,    1 },
    { "win.height",     SLE_SYS_WIN_HEIGHT,  1 },
    { "win.title",      SLE_SYS_WIN_TITLE,   2 },

    /* 绘图 */
    { "gfx.clear",      SLE_SYS_GFX_CLEAR,   1 },
    { "gfx.rect",       SLE_SYS_GFX_RECT,    5 },
    { "gfx.frame",      SLE_SYS_GFX_FRAME,   6 },
    { "gfx.text",       SLE_SYS_GFX_TEXT,    4 },
    { "gfx.text_width", SLE_SYS_GFX_TWIDTH,  1 },
    { "gfx.line",       SLE_SYS_GFX_LINE,    5 },
    { "gfx.circle",     SLE_SYS_GFX_CIRCLE,  4 },
    { "gfx.flush",      SLE_SYS_GFX_FLUSH,   0 },

    /* 图片（BMP 解码由宿主完成，句柄在单个应用窗口内有效） */
    { "img.load",       SLE_SYS_IMG_LOAD,        1 },
    { "img.width",      SLE_SYS_IMG_WIDTH,       1 },
    { "img.height",     SLE_SYS_IMG_HEIGHT,      1 },
    { "img.draw",       SLE_SYS_IMG_DRAW,        3 },
    { "img.draw_scaled", SLE_SYS_IMG_DRAW_SCALED, 5 },
    { "img.draw_circle", SLE_SYS_IMG_DRAW_CIRCLE, 4 },
    { "img.free",       SLE_SYS_IMG_FREE,        1 },

    /* 输入 */
    { "event.poll",     SLE_SYS_EVT_POLL,    0 },
    { "event.wait",     SLE_SYS_EVT_WAIT,    1 },

    /* 内置 */
    { "len",            SLE_BIF_LEN,         1 },
    { "str",            SLE_BIF_STR,         1 },
    { "int",            SLE_BIF_INT,         1 },
    { "substr",         SLE_BIF_SUBSTR,      3 },
    { "index_of",       SLE_BIF_INDEXOF,     2 },
    { "upper",          SLE_BIF_UPPER,       1 },
    { "lower",          SLE_BIF_LOWER,       1 },
    { "abs",            SLE_BIF_ABS,         1 },
    { "min",            SLE_BIF_MIN,         2 },
    { "max",            SLE_BIF_MAX,         2 },
    { "array",          SLE_BIF_ARRAY,       1 },
    { "push",           SLE_BIF_PUSH,        2 },
    { "pop",            SLE_BIF_POP,         1 },
    { "range",          SLE_BIF_RANGE,       2 },
    { "random",         SLE_BIF_RANDOM,      1 },
    { "typeof",         SLE_BIF_TYPEOF,      1 },
    { "trim",           SLE_BIF_TRIM,        1 },
    { "replace",        SLE_BIF_REPLACE,     3 },
    { "split",          SLE_BIF_SPLIT,       2 },
    { "join",           SLE_BIF_JOIN,        2 },
    { "str_at",         SLE_BIF_STR_AT,      2 },
    { "ord",            SLE_BIF_ORD,         1 },
    { "chr",            SLE_BIF_CHR,         1 },
    { "clamp",          SLE_BIF_CLAMP,       3 },
    { "sign",           SLE_BIF_SIGN,        1 },
    { "hex",            SLE_BIF_HEX,         1 },
    { "bin",            SLE_BIF_BIN,         1 },
    { 0, 0, 0 }
};

static const char *g_namespaces[] = {
    "fs", "cfg", "time", "os", "win", "gfx", "event", "img",
    "audio", "media", 0
};

static int is_namespace(const char *name) {
    for (int i = 0; g_namespaces[i]; i++) {
        if (strcmp(g_namespaces[i], name) == 0) return 1;
    }
    return 0;
}

static const BuiltinDef *builtin_find(const char *name) {
    for (int i = 0; g_builtins[i].name; i++) {
        if (strcmp(g_builtins[i].name, name) == 0) return &g_builtins[i];
    }
    return 0;
}

/* ---------- 代码缓冲 ---------- */

static void emit_u8(Gen *g, uint8_t v) {
    Compiler *cc = g->cc;
    if (cc->code_len + 1 > cc->code_cap) {
        uint32_t next = cc->code_cap ? cc->code_cap * 2 : 4096;
        uint8_t *nb = (uint8_t *)xmalloc(next);
        if (cc->code) {
            memcpy(nb, cc->code, cc->code_len);
            free(cc->code);
        }
        cc->code = nb;
        cc->code_cap = next;
    }
    cc->code[cc->code_len++] = v;
}

static void emit_u16(Gen *g, uint16_t v) {
    emit_u8(g, (uint8_t)(v & 0xFF));
    emit_u8(g, (uint8_t)((v >> 8) & 0xFF));
}

static void emit_u32(Gen *g, uint32_t v) {
    emit_u8(g, (uint8_t)(v & 0xFF));
    emit_u8(g, (uint8_t)((v >> 8) & 0xFF));
    emit_u8(g, (uint8_t)((v >> 16) & 0xFF));
    emit_u8(g, (uint8_t)((v >> 24) & 0xFF));
}

static void emit_i64(Gen *g, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        emit_u8(g, (uint8_t)((u >> (i * 8)) & 0xFF));
    }
}

static uint32_t here(Gen *g) {
    return g->cc->code_len;
}

static void patch_u32(Gen *g, uint32_t at, uint32_t value) {
    Compiler *cc = g->cc;
    if (at + 4 > cc->code_len) {
        parse_fail(0, 0, "internal: patch outside code");
    }
    cc->code[at + 0] = (uint8_t)(value & 0xFF);
    cc->code[at + 1] = (uint8_t)((value >> 8) & 0xFF);
    cc->code[at + 2] = (uint8_t)((value >> 16) & 0xFF);
    cc->code[at + 3] = (uint8_t)((value >> 24) & 0xFF);
}

static void depth_push(Gen *g, int n) {
    g->depth += n;
    if (g->depth > g->max_depth) g->max_depth = g->depth;
}

static void depth_pop(Gen *g, int n) {
    g->depth -= n;
    if (g->depth < 0) g->depth = 0;
}

/* ---------- 作用域 ---------- */

static int var_find(Gen *g, const char *name) {
    for (int i = g->var_count - 1; i >= 0; i--) {
        if (!g->vars[i].name) continue;
        if (strcmp(g->vars[i].name, name) == 0) return g->vars[i].slot;
    }
    return -1;
}

static void var_ensure_capacity(Gen *g) {
    if (g->var_count < g->var_cap) return;

    int next = g->var_cap ? g->var_cap * 2 : 16;
    VarBind *nv = (VarBind *)xmalloc(sizeof(VarBind) * (size_t)next);
    for (int i = 0; i < g->var_count; i++) nv[i] = g->vars[i];
    if (g->vars) free(g->vars);
    g->vars = nv;
    g->var_cap = next;
}

static int var_declare(Gen *g, const char *name, int line, int col) {
    if (var_find(g, name) >= 0) {
        parse_fail(line, col, "variable '%s' already declared in this scope", name);
    }

    var_ensure_capacity(g);

    int slot = g->var_count;
    g->vars[g->var_count].name = xstrdup(name);
    g->vars[g->var_count].slot = slot;
    g->var_count++;
    return slot;
}

/* 分配一个无名的隐藏槽（for 循环上界等），var_find 会跳过它 */
static int var_raw_slot(Gen *g) {
    var_ensure_capacity(g);

    int slot = g->var_count;
    g->vars[g->var_count].name = 0;
    g->vars[g->var_count].slot = slot;
    g->var_count++;
    return slot;
}

static int global_find(Compiler *cc, const char *name) {
    for (int i = 0; i < cc->global_count; i++) {
        if (strcmp(cc->global_names[i], name) == 0) return cc->global_slots[i];
    }
    return -1;
}

static int global_declare(Compiler *cc, const char *name, int line, int col) {
    int existing = global_find(cc, name);
    if (existing >= 0) {
        parse_fail(line, col, "global '%s' already declared", name);
    }
    if (cc->global_count >= SLYZ_MAX_GLOBALS) {
        parse_fail(line, col, "too many globals (max %d)", SLYZ_MAX_GLOBALS);
    }
    char **nn = (char **)xmalloc(sizeof(char *) * (size_t)(cc->global_count + 1));
    int *ns = (int *)xmalloc(sizeof(int) * (size_t)(cc->global_count + 1));
    for (int i = 0; i < cc->global_count; i++) {
        nn[i] = cc->global_names[i];
        ns[i] = cc->global_slots[i];
    }
    nn[cc->global_count] = xstrdup(name);
    ns[cc->global_count] = cc->global_count;
    if (cc->global_names) free(cc->global_names);
    if (cc->global_slots) free(cc->global_slots);
    cc->global_names = nn;
    cc->global_slots = ns;
    cc->global_count++;
    return ns[cc->global_count - 1];
}

static int fn_find(Compiler *cc, const char *name) {
    for (int i = 0; i < cc->func_count; i++) {
        if (strcmp(cc->funcs[i]->name, name) == 0) return i;
    }
    return -1;
}

/* ---------- 局部槽预扫描 ---------- */

static void scan_stmt(Stmt *s, int *cur, int *max);

static void scan_block(Stmt *b, int *cur, int *max) {
    int mark = *cur;
    for (int i = 0; i < b->count; i++) scan_stmt(b->body[i], cur, max);
    *cur = mark;
}

static void scan_stmt(Stmt *s, int *cur, int *max) {
    if (!s) return;

    switch (s->kind) {
        case ST_LET:
            (*cur)++;
            if (*cur > *max) *max = *cur;
            break;

        case ST_IF:
            scan_block(s->then_s, cur, max);
            if (s->else_s) {
                if (s->else_s->kind == ST_IF) scan_stmt(s->else_s, cur, max);
                else scan_block(s->else_s, cur, max);
            }
            break;

        case ST_WHILE:
            scan_block(s->then_s, cur, max);
            break;

        case ST_FOR: {
            int mark = *cur;
            (*cur) += 2;   /* 循环变量 + 上界 */
            if (*cur > *max) *max = *cur;
            scan_block(s->then_s, cur, max);
            *cur = mark;
            break;
        }

        case ST_BLOCK:
            scan_block(s, cur, max);
            break;

        default:
            break;
    }
}

/* ---------- 表达式 ---------- */

static void gen_expr(Gen *g, Expr *e);
static void gen_stmt(Gen *g, Stmt *s);

static void gen_call(Gen *g, Expr *e);

static void gen_binary_short(Gen *g, Expr *e, int is_and) {
    gen_expr(g, e->a);
    emit_u8(g, SLE_OP_DUP);
    depth_push(g, 1);
    emit_u8(g, is_and ? SLE_OP_JZ : SLE_OP_JNZ);
    uint32_t to_short = here(g);
    emit_u32(g, 0);
    depth_pop(g, 1);

    emit_u8(g, SLE_OP_POP);
    depth_pop(g, 1);

    gen_expr(g, e->b);
    emit_u8(g, SLE_OP_JMP);
    uint32_t to_end = here(g);
    emit_u32(g, 0);

    uint32_t short_at = here(g);
    patch_u32(g, to_short, short_at);

    uint32_t end_at = here(g);
    patch_u32(g, to_end, end_at);
}

static void gen_array(Gen *g, Expr *e) {
    for (int i = 0; i < e->count; i++) gen_expr(g, e->list[i]);
    emit_u8(g, SLE_OP_MAKE_ARR);
    emit_u32(g, (uint32_t)e->count);
    if (e->count == 0) {
        depth_push(g, 1);
    } else {
        depth_pop(g, e->count - 1);
    }
}

static void gen_index_get(Gen *g, Expr *e) {
    gen_expr(g, e->a);
    gen_expr(g, e->b);
    emit_u8(g, SLE_OP_IDX_GET);
    depth_pop(g, 1);
}

static void gen_ident_load(Gen *g, const char *name, int line, int col) {
    int slot = g->in_top ? -1 : var_find(g, name);
    if (slot >= 0) {
        emit_u8(g, SLE_OP_LOAD_L);
        emit_u32(g, (uint32_t)slot);
        depth_push(g, 1);
        return;
    }

    int gslot = global_find(g->cc, name);
    if (gslot >= 0) {
        emit_u8(g, SLE_OP_LOAD_G);
        emit_u32(g, (uint32_t)gslot);
        depth_push(g, 1);
        return;
    }

    if (fn_find(g->cc, name) >= 0) {
        parse_fail(line, col, "function '%s' must be called with ()", name);
    }

    parse_fail(line, col, "undefined variable '%s'", name);
}

static void gen_assign(Gen *g, Expr *e) {
    Expr *target = e->a;

    if (target->kind == EX_IDENT) {
        int slot = g->in_top ? -1 : var_find(g, target->name);
        int is_global = 0;

        if (slot < 0) {
            slot = global_find(g->cc, target->name);
            if (slot < 0) {
                parse_fail(target->line, target->col,
                           "assignment to undefined variable '%s'", target->name);
            }
            is_global = 1;
        }

        if (e->assign_op == ASSIGN_SET) {
            gen_expr(g, e->b);
        } else {
            if (is_global) {
                emit_u8(g, SLE_OP_LOAD_G);
                emit_u32(g, (uint32_t)slot);
            } else {
                emit_u8(g, SLE_OP_LOAD_L);
                emit_u32(g, (uint32_t)slot);
            }
            depth_push(g, 1);

            gen_expr(g, e->b);

            uint8_t op = SLE_OP_ADD;
            if (e->assign_op == ASSIGN_SUB) op = SLE_OP_SUB;
            else if (e->assign_op == ASSIGN_MUL) op = SLE_OP_MUL;
            else if (e->assign_op == ASSIGN_DIV) op = SLE_OP_DIV;
            else if (e->assign_op == ASSIGN_MOD) op = SLE_OP_MOD;
            emit_u8(g, op);
            depth_pop(g, 1);
        }

        if (is_global) {
            emit_u8(g, SLE_OP_STORE_G);
            emit_u32(g, (uint32_t)slot);
            emit_u8(g, SLE_OP_LOAD_G);
            emit_u32(g, (uint32_t)slot);
        } else {
            emit_u8(g, SLE_OP_STORE_L);
            emit_u32(g, (uint32_t)slot);
            emit_u8(g, SLE_OP_LOAD_L);
            emit_u32(g, (uint32_t)slot);
        }
        depth_pop(g, 1);
        depth_push(g, 1);
        return;
    }

    if (target->kind == EX_INDEX) {
        if (e->assign_op == ASSIGN_SET) {
            gen_expr(g, target->a);
            gen_expr(g, target->b);
            gen_expr(g, e->b);
            emit_u8(g, SLE_OP_IDX_SET);
            depth_pop(g, 3);
        } else {
            gen_expr(g, target->a);
            gen_expr(g, target->b);
            emit_u8(g, SLE_OP_DUP2);
            depth_push(g, 2);
            emit_u8(g, SLE_OP_IDX_GET);
            depth_pop(g, 1);

            gen_expr(g, e->b);
            uint8_t op = SLE_OP_ADD;
            if (e->assign_op == ASSIGN_SUB) op = SLE_OP_SUB;
            else if (e->assign_op == ASSIGN_MUL) op = SLE_OP_MUL;
            else if (e->assign_op == ASSIGN_DIV) op = SLE_OP_DIV;
            else if (e->assign_op == ASSIGN_MOD) op = SLE_OP_MOD;
            emit_u8(g, op);
            depth_pop(g, 1);

            emit_u8(g, SLE_OP_IDX_SET);
            depth_pop(g, 3);
        }
        emit_u8(g, SLE_OP_PUSH_NIL);
        depth_push(g, 1);
        return;
    }

    parse_fail(target->line, target->col, "invalid assignment target");
}

static void gen_postfix(Gen *g, Expr *e) {
    Expr *target = e->a;

    if (target->kind != EX_IDENT) {
        parse_fail(e->line, e->col, "'++' and '--' only apply to variables");
    }

    int slot = g->in_top ? -1 : var_find(g, target->name);
    int is_global = 0;
    if (slot < 0) {
        slot = global_find(g->cc, target->name);
        if (slot < 0) {
            parse_fail(target->line, target->col,
                       "undefined variable '%s'", target->name);
        }
        is_global = 1;
    }

    if (is_global) { emit_u8(g, SLE_OP_LOAD_G); emit_u32(g, (uint32_t)slot); }
    else { emit_u8(g, SLE_OP_LOAD_L); emit_u32(g, (uint32_t)slot); }
    depth_push(g, 1);

    emit_u8(g, SLE_OP_PUSH_I);
    emit_i64(g, 1);
    depth_push(g, 1);

    emit_u8(g, e->op > 0 ? SLE_OP_ADD : SLE_OP_SUB);
    depth_pop(g, 1);

    if (is_global) { emit_u8(g, SLE_OP_STORE_G); emit_u32(g, (uint32_t)slot); }
    else { emit_u8(g, SLE_OP_STORE_L); emit_u32(g, (uint32_t)slot); }
    depth_pop(g, 1);

    if (is_global) { emit_u8(g, SLE_OP_LOAD_G); emit_u32(g, (uint32_t)slot); }
    else { emit_u8(g, SLE_OP_LOAD_L); emit_u32(g, (uint32_t)slot); }
    depth_push(g, 1);
}

static void gen_expr(Gen *g, Expr *e) {
    if (!e) {
        emit_u8(g, SLE_OP_PUSH_NIL);
        depth_push(g, 1);
        return;
    }

    switch (e->kind) {
        case EX_INT:
            emit_u8(g, SLE_OP_PUSH_I);
            emit_i64(g, e->ival);
            depth_push(g, 1);
            return;

        case EX_STR: {
            int idx = 0;
            uint32_t off = consts_add(&g->cc->consts, e->sval, e->slen, &idx);
            emit_u8(g, SLE_OP_PUSH_S);
            emit_u32(g, off);
            depth_push(g, 1);
            return;
        }

        case EX_BOOL:
            emit_u8(g, e->ival ? SLE_OP_PUSH_TRUE : SLE_OP_PUSH_FALSE);
            depth_push(g, 1);
            return;

        case EX_NIL:
            emit_u8(g, SLE_OP_PUSH_NIL);
            depth_push(g, 1);
            return;

        case EX_IDENT:
            gen_ident_load(g, e->name, e->line, e->col);
            return;

        case EX_BIN: {
            if (e->op == 10) { gen_binary_short(g, e, 1); return; }
            if (e->op == 11) { gen_binary_short(g, e, 0); return; }

            gen_expr(g, e->a);
            gen_expr(g, e->b);

            uint8_t op = SLE_OP_ADD;
            if (e->op == '-') op = SLE_OP_SUB;
            else if (e->op == '*') op = SLE_OP_MUL;
            else if (e->op == '/') op = SLE_OP_DIV;
            else if (e->op == '%') op = SLE_OP_MOD;
            else if (e->op == 1) op = SLE_OP_EQ;
            else if (e->op == 2) op = SLE_OP_NE;
            else if (e->op == 3) op = SLE_OP_LT;
            else if (e->op == 4) op = SLE_OP_LE;
            else if (e->op == 5) op = SLE_OP_GT;
            else if (e->op == 6) op = SLE_OP_GE;

            emit_u8(g, op);
            depth_pop(g, 1);
            return;
        }

        case EX_UN:
            gen_expr(g, e->a);
            emit_u8(g, e->op == '-' ? SLE_OP_NEG : SLE_OP_NOT);
            return;

        case EX_CALL:
            gen_call(g, e);
            return;

        case EX_INDEX:
            gen_index_get(g, e);
            return;

        case EX_ARRAY:
            gen_array(g, e);
            return;

        case EX_ASSIGN:
            gen_assign(g, e);
            return;

        case EX_POSTFIX:
            gen_postfix(g, e);
            return;

        case EX_MEMBER: {
            if (e->a->kind == EX_IDENT && is_namespace(e->a->name)) {
                parse_fail(e->line, e->col,
                           "namespace '%s' cannot be used as a value", e->a->name);
            }
            if (strcmp(e->member, "length") != 0) {
                parse_fail(e->line, e->col,
                           "unknown property '%s' (only 'length' is supported)", e->member);
            }
            gen_expr(g, e->a);
            emit_u8(g, SLE_OP_SYS);
            emit_u16(g, (uint16_t)SLE_BIF_LEN);
            emit_u16(g, 1);
            return;
        }

        default:
            parse_fail(e->line, e->col, "internal: unknown expression kind %d", e->kind);
    }
}

/* ---------- 调用 ---------- */

static void gen_args(Gen *g, Expr *e) {
    for (int i = 0; i < e->count; i++) gen_expr(g, e->list[i]);
}

static void check_argc(const BuiltinDef *b, Expr *call) {
    if (b->argc >= 0 && call->count != b->argc) {
        parse_fail(call->line, call->col,
                   "'%s' expects %d argument(s), got %d",
                   b->name, b->argc, call->count);
    }
}

/* 顶层调用表：记录名字与位置，编译结束后回填 */
typedef struct {
    uint32_t at;
    char    *name;
    int      line;
    int      col;
} TopCall;

static TopCall *g_top_calls = 0;
static int      g_top_call_count = 0;
static int      g_top_call_cap = 0;

static void top_call_add(uint32_t at, const char *name, int line, int col) {
    if (g_top_call_count >= g_top_call_cap) {
        int next = g_top_call_cap ? g_top_call_cap * 2 : 8;
        TopCall *nt = (TopCall *)xmalloc(sizeof(TopCall) * (size_t)next);
        for (int i = 0; i < g_top_call_count; i++) nt[i] = g_top_calls[i];
        if (g_top_calls) free(g_top_calls);
        g_top_calls = nt;
        g_top_call_cap = next;
    }
    g_top_calls[g_top_call_count].at = at;
    g_top_calls[g_top_call_count].name = xstrdup(name);
    g_top_calls[g_top_call_count].line = line;
    g_top_calls[g_top_call_count].col = col;
    g_top_call_count++;
}

static void gen_call(Gen *g, Expr *e) {
    Expr *callee = e->a;

    if (callee->kind == EX_IDENT) {
        FnDef *fn = 0;
        int fi = fn_find(g->cc, callee->name);
        if (fi >= 0) fn = g->cc->funcs[fi];

        if (fn) {
            if (e->count != fn->param_count) {
                parse_fail(e->line, e->col,
                           "function '%s' expects %d argument(s), got %d",
                           fn->name, fn->param_count, e->count);
            }
            gen_args(g, e);
            emit_u8(g, SLE_OP_CALL);
            uint32_t at = here(g);
            emit_u32(g, 0);
            emit_u32(g, (uint32_t)e->count);

            if (g->fn) {
                if (g->fn->patch_count >= g->fn->patch_cap) {
                    int next = g->fn->patch_cap ? g->fn->patch_cap * 2 : 8;
                    struct CallPatch *np = (struct CallPatch *)xmalloc(
                        sizeof(struct CallPatch) * (size_t)next);
                    for (int i = 0; i < g->fn->patch_count; i++) np[i] = g->fn->patches[i];
                    if (g->fn->patches) free(g->fn->patches);
                    g->fn->patches = np;
                    g->fn->patch_cap = next;
                }
                g->fn->patches[g->fn->patch_count].at = at;
                g->fn->patches[g->fn->patch_count].name = xstrdup(fn->name);
                g->fn->patch_count++;
            } else {
                top_call_add(at, fn->name, e->line, e->col);
            }

            depth_pop(g, e->count);
            depth_push(g, 1);
            return;
        }

        const BuiltinDef *b = builtin_find(callee->name);
        if (b) {
            check_argc(b, e);
            gen_args(g, e);
            emit_u8(g, SLE_OP_SYS);
            emit_u16(g, (uint16_t)b->id);
            emit_u16(g, (uint16_t)e->count);
            depth_pop(g, e->count);
            depth_push(g, 1);
            return;
        }

        parse_fail(e->line, e->col, "undefined function '%s'", callee->name);
    }

    if (callee->kind == EX_MEMBER) {
        if (callee->a->kind == EX_IDENT && is_namespace(callee->a->name)) {
            char full[NAME_MAX_LEN];
            snprintf(full, sizeof(full), "%s.%s", callee->a->name, callee->member);

            const BuiltinDef *b = builtin_find(full);
            if (!b) {
                parse_fail(e->line, e->col, "unknown system call '%s'", full);
            }
            check_argc(b, e);

            if (strcmp(callee->a->name, "gfx") == 0 ||
                strcmp(callee->a->name, "win") == 0 ||
                strcmp(callee->a->name, "event") == 0) {
                g->cc->uses_gui = 1;
            }

            gen_args(g, e);
            emit_u8(g, SLE_OP_SYS);
            emit_u16(g, (uint16_t)b->id);
            emit_u16(g, (uint16_t)e->count);
            depth_pop(g, e->count);
            depth_push(g, 1);
            return;
        }

        /* 方法调用：把接收者作为第一个参数 */
        const char *m = callee->member;

        if (strcmp(m, "at") == 0) {
            if (e->count != 1) {
                parse_fail(e->line, e->col, "'at' expects 1 index argument");
            }
            gen_expr(g, callee->a);
            gen_expr(g, e->list[0]);
            emit_u8(g, SLE_OP_IDX_GET);
            depth_pop(g, 1);
            return;
        }

        if (strcmp(m, "set") == 0) {
            if (e->count != 2) {
                parse_fail(e->line, e->col, "'set' expects index and value");
            }
            gen_expr(g, callee->a);
            gen_expr(g, e->list[0]);
            gen_expr(g, e->list[1]);
            emit_u8(g, SLE_OP_IDX_SET);
            depth_pop(g, 3);
            emit_u8(g, SLE_OP_PUSH_NIL);
            depth_push(g, 1);
            return;
        }

        char full[NAME_MAX_LEN];
        snprintf(full, sizeof(full), "%s", m);
        const BuiltinDef *b = builtin_find(full);
        if (!b) {
            parse_fail(e->line, e->col, "unknown method '%s'", m);
        }
        if (b->argc >= 0 && e->count + 1 != b->argc) {
            parse_fail(e->line, e->col,
                       "method '%s' expects %d argument(s), got %d",
                       m, b->argc - 1, e->count);
        }

        gen_expr(g, callee->a);
        gen_args(g, e);
        emit_u8(g, SLE_OP_SYS);
        emit_u16(g, (uint16_t)b->id);
        emit_u16(g, (uint16_t)(e->count + 1));
        depth_pop(g, e->count + 1);
        depth_push(g, 1);
        return;
    }

    parse_fail(e->line, e->col, "expression is not callable");
}

/* ---------- 语句 ---------- */

static void gen_block_scoped(Gen *g, Stmt *b) {
    int mark = g->var_count;
    for (int i = 0; i < b->count; i++) gen_stmt(g, b->body[i]);
    g->var_count = mark;
}

static void patch_loop_jumps(Gen *g, LoopCtx *loop, uint32_t break_to, uint32_t continue_to) {
    for (int i = 0; i < loop->breaks.n; i++) {
        patch_u32(g, loop->breaks.v[i].at, break_to);
    }
    for (int i = 0; i < loop->continues.n; i++) {
        patch_u32(g, loop->continues.v[i].at, continue_to);
    }
}

static void gen_stmt(Gen *g, Stmt *s) {
    if (!s) return;

    switch (s->kind) {
        case ST_LET: {
            int slot = -1;
            int is_global = 0;

            if (g->in_top) {
                slot = global_find(g->cc, s->name);
                if (slot < 0) slot = global_declare(g->cc, s->name, s->line, s->col);
                is_global = 1;
            } else {
                slot = var_declare(g, s->name, s->line, s->col);
            }

            if (s->e) gen_expr(g, s->e);
            else { emit_u8(g, SLE_OP_PUSH_NIL); depth_push(g, 1); }

            if (is_global) {
                emit_u8(g, SLE_OP_STORE_G);
                emit_u32(g, (uint32_t)slot);
            } else {
                emit_u8(g, SLE_OP_STORE_L);
                emit_u32(g, (uint32_t)slot);
            }
            depth_pop(g, 1);
            return;
        }

        case ST_EXPR:
            gen_expr(g, s->e);
            emit_u8(g, SLE_OP_POP);
            depth_pop(g, 1);
            return;

        case ST_IF: {
            gen_expr(g, s->e);
            emit_u8(g, SLE_OP_JZ);
            uint32_t to_else = here(g);
            emit_u32(g, 0);
            depth_pop(g, 1);

            int entered = g->terminated;
            g->terminated = 0;
            gen_block_scoped(g, s->then_s);

            uint32_t to_end = 0;
            int need_end_jump = 0;
            if (s->else_s) {
                emit_u8(g, SLE_OP_JMP);
                to_end = here(g);
                emit_u32(g, 0);
                need_end_jump = 1;
            }

            patch_u32(g, to_else, here(g));

            if (s->else_s) {
                g->terminated = 0;
                if (s->else_s->kind == ST_IF) gen_stmt(g, s->else_s);
                else gen_block_scoped(g, s->else_s);
                if (need_end_jump) patch_u32(g, to_end, here(g));
            }

            if (!entered) g->terminated = 0;
            return;
        }

        case ST_WHILE: {
            uint32_t cond_at = here(g);
            gen_expr(g, s->e);
            emit_u8(g, SLE_OP_JZ);
            uint32_t to_end = here(g);
            emit_u32(g, 0);
            depth_pop(g, 1);

            LoopCtx loop;
            memset(&loop, 0, sizeof(loop));
            loop.prev = g->loop;
            loop.is_for = 0;
            g->loop = &loop;

            int saved_term = g->terminated;
            g->terminated = 0;
            gen_block_scoped(g, s->then_s);
            g->terminated = saved_term;

            g->loop = loop.prev;

            emit_u8(g, SLE_OP_JMP);
            emit_u32(g, cond_at);

            uint32_t end_at = here(g);
            patch_u32(g, to_end, end_at);
            patch_loop_jumps(g, &loop, end_at, cond_at);

            if (loop.breaks.v) free(loop.breaks.v);
            if (loop.continues.v) free(loop.continues.v);
            return;
        }

        case ST_FOR: {
            int mark = g->var_count;
            int var_slot = var_declare(g, s->name, s->line, s->col);
            int end_slot = var_raw_slot(g);

            gen_expr(g, s->e);
            emit_u8(g, SLE_OP_STORE_L);
            emit_u32(g, (uint32_t)var_slot);
            depth_pop(g, 1);

            gen_expr(g, s->e2);
            emit_u8(g, SLE_OP_STORE_L);
            emit_u32(g, (uint32_t)end_slot);
            depth_pop(g, 1);

            uint32_t cond_at = here(g);
            emit_u8(g, SLE_OP_LOAD_L);
            emit_u32(g, (uint32_t)var_slot);
            depth_push(g, 1);
            emit_u8(g, SLE_OP_LOAD_L);
            emit_u32(g, (uint32_t)end_slot);
            depth_push(g, 1);
            emit_u8(g, SLE_OP_LT);
            depth_pop(g, 1);
            emit_u8(g, SLE_OP_JZ);
            uint32_t to_end = here(g);
            emit_u32(g, 0);
            depth_pop(g, 1);

            LoopCtx loop;
            memset(&loop, 0, sizeof(loop));
            loop.prev = g->loop;
            loop.is_for = 1;
            g->loop = &loop;

            int saved_term = g->terminated;
            g->terminated = 0;
            gen_block_scoped(g, s->then_s);
            g->terminated = saved_term;

            g->loop = loop.prev;

            uint32_t cont_at = here(g);
            emit_u8(g, SLE_OP_LOAD_L);
            emit_u32(g, (uint32_t)var_slot);
            depth_push(g, 1);
            emit_u8(g, SLE_OP_PUSH_I);
            emit_i64(g, 1);
            depth_push(g, 1);
            emit_u8(g, SLE_OP_ADD);
            depth_pop(g, 1);
            emit_u8(g, SLE_OP_STORE_L);
            emit_u32(g, (uint32_t)var_slot);
            depth_pop(g, 1);

            emit_u8(g, SLE_OP_JMP);
            emit_u32(g, cond_at);

            uint32_t end_at = here(g);
            patch_u32(g, to_end, end_at);
            patch_loop_jumps(g, &loop, end_at, cont_at);

            if (loop.breaks.v) free(loop.breaks.v);
            if (loop.continues.v) free(loop.continues.v);

            g->var_count = mark;
            return;
        }

        case ST_RETURN: {
            if (!g->fn) {
                parse_fail(s->line, s->col, "'return' outside function");
            }
            if (s->e) gen_expr(g, s->e);
            else { emit_u8(g, SLE_OP_PUSH_NIL); depth_push(g, 1); }
            emit_u8(g, SLE_OP_RET);
            g->depth = 0;
            g->terminated = 1;
            return;
        }

        case ST_BREAK:
            if (!g->loop) parse_fail(s->line, s->col, "'break' outside loop");
            emit_u8(g, SLE_OP_JMP);
            pv_push(&g->loop->breaks, here(g));
            emit_u32(g, 0);
            g->terminated = 1;
            return;

        case ST_CONTINUE:
            if (!g->loop) parse_fail(s->line, s->col, "'continue' outside loop");
            emit_u8(g, SLE_OP_JMP);
            pv_push(&g->loop->continues, here(g));
            emit_u32(g, 0);
            g->terminated = 1;
            return;

        case ST_BLOCK: {
            int saved = g->terminated;
            g->terminated = 0;
            gen_block_scoped(g, s);
            g->terminated = saved;
            return;
        }

        default:
            parse_fail(s->line, s->col, "internal: unknown statement kind %d", s->kind);
    }
}

/* ---------- 函数编译 ---------- */

static void scan_function_locals(FnDef *fn) {
    int cur = fn->param_count;
    int max = fn->param_count;
    scan_block(fn->body, &cur, &max);
    fn->local_count = max;
}

static void gen_function(Gen *g, FnDef *fn) {
    fn->entry = here(g);
    fn->compiled = 1;

    g->fn = fn;
    g->vars = 0;
    g->var_count = 0;
    g->var_cap = 0;
    g->depth = 0;
    g->max_depth = 0;
    g->terminated = 0;
    g->loop = 0;
    g->in_top = 0;

    for (int i = 0; i < fn->param_count; i++) {
        var_declare(g, fn->params[i], fn->line, fn->col);
    }

    int locals = fn->local_count - fn->param_count;
    if (locals < 0) locals = 0;
    for (int i = 0; i < locals; i++) {
        emit_u8(g, SLE_OP_PUSH_NIL);
        depth_push(g, 1);
    }

    gen_block_scoped(g, fn->body);

    if (!g->terminated) {
        emit_u8(g, SLE_OP_PUSH_NIL);
        depth_push(g, 1);
        emit_u8(g, SLE_OP_RET);
        g->depth = 0;
    }

    if (g->max_depth > g->cc->max_stack) g->cc->max_stack = g->max_depth;

    /* 变量名由编译进程统一回收：这里只重置变量表本身，
     * 避免逐个追踪块作用域回滚后仍被引用的名字。 */
    if (g->vars) free(g->vars);
    g->vars = 0;
    g->var_count = 0;
    g->var_cap = 0;
    g->fn = 0;
}

/* ---------- 顶层 ---------- */

static void scan_top_globals(Compiler *cc) {
    for (int i = 0; i < cc->program_count; i++) {
        Stmt *s = cc->program[i];
        if (s->kind == ST_LET) {
            if (global_find(cc, s->name) < 0) {
                global_declare(cc, s->name, s->line, s->col);
            }
        }
    }
}

static void resolve_top_calls(Gen *g) {
    for (int i = 0; i < g_top_call_count; i++) {
        int fi = fn_find(g->cc, g_top_calls[i].name);
        if (fi < 0) {
            parse_fail(g_top_calls[i].line, g_top_calls[i].col,
                       "undefined function '%s'", g_top_calls[i].name);
        }
        patch_u32(g, g_top_calls[i].at, g->cc->funcs[fi]->entry);
    }
}

static void resolve_function_calls(Gen *g) {
    for (int i = 0; i < g->cc->func_count; i++) {
        FnDef *fn = g->cc->funcs[i];
        for (int k = 0; k < fn->patch_count; k++) {
            int fi = fn_find(g->cc, fn->patches[k].name);
            if (fi < 0) {
                parse_fail(fn->line, fn->col,
                           "undefined function '%s'", fn->patches[k].name);
            }
            patch_u32(g, fn->patches[k].at, g->cc->funcs[fi]->entry);
        }
    }
}

void codegen_program(Compiler *cc) {
    Gen g;
    memset(&g, 0, sizeof(g));
    g.cc = cc;
    g.in_top = 1;

    /* 函数重名检查 */
    for (int i = 0; i < cc->func_count; i++) {
        for (int k = i + 1; k < cc->func_count; k++) {
            if (strcmp(cc->funcs[i]->name, cc->funcs[k]->name) == 0) {
                parse_fail(cc->funcs[k]->line, cc->funcs[k]->col,
                           "function '%s' already defined at line %d",
                           cc->funcs[k]->name, cc->funcs[i]->line);
            }
        }
        for (int k = 0; k < cc->funcs[i]->param_count; k++) {
            for (int m = k + 1; m < cc->funcs[i]->param_count; m++) {
                if (strcmp(cc->funcs[i]->params[k], cc->funcs[i]->params[m]) == 0) {
                    parse_fail(cc->funcs[i]->line, cc->funcs[i]->col,
                               "duplicate parameter '%s' in function '%s'",
                               cc->funcs[i]->params[k], cc->funcs[i]->name);
                }
            }
        }
    }

    scan_top_globals(cc);

    for (int i = 0; i < cc->func_count; i++) {
        scan_function_locals(cc->funcs[i]);
    }

    cc->entry_offset = here(&g);

    for (int i = 0; i < cc->program_count; i++) {
        gen_stmt(&g, cc->program[i]);
    }

    /* 顶层语句执行完后调用 main（如果存在） */
    int main_index = fn_find(cc, "main");
    if (main_index >= 0) {
        FnDef *main_fn = cc->funcs[main_index];
        if (main_fn->param_count != 0) {
            parse_fail(main_fn->line, main_fn->col,
                       "'main' must not take parameters (got %d)", main_fn->param_count);
        }
        emit_u8(&g, SLE_OP_CALL);
        uint32_t at = here(&g);
        emit_u32(&g, 0);
        emit_u32(&g, 0);
        top_call_add(at, "main", main_fn->line, main_fn->col);
    }

    emit_u8(&g, SLE_OP_HALT);

    for (int i = 0; i < cc->func_count; i++) {
        gen_function(&g, cc->funcs[i]);
    }

    resolve_function_calls(&g);
    resolve_top_calls(&g);

    if (g.max_depth > cc->max_stack) cc->max_stack = g.max_depth;

    for (int i = 0; i < g_top_call_count; i++) free(g_top_calls[i].name);
    if (g_top_calls) free(g_top_calls);
    g_top_calls = 0;
    g_top_call_count = 0;
    g_top_call_cap = 0;

    if (cc->error_count) {
        parse_fail(0, 0, "compilation aborted");
    }
}

/* ---------- 镜像输出 ---------- */

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int compile_image(Compiler *cc, uint8_t **out, uint32_t *out_size) {
    uint32_t const_size = cc->consts.len;
    uint32_t code_size = cc->code_len;

    if (const_size > SLYZ_MAX_CONST) {
        diag_error(0, 0, "constant pool is %u bytes, limit is %u",
                   (unsigned)const_size, (unsigned)SLYZ_MAX_CONST);
        return 0;
    }
    if (code_size > SLYZ_MAX_CODE) {
        diag_error(0, 0, "bytecode is %u bytes, limit is %u",
                   (unsigned)code_size, (unsigned)SLYZ_MAX_CODE);
        return 0;
    }
    if (cc->entry_offset >= code_size) {
        diag_error(0, 0, "internal: entry point outside code");
        return 0;
    }

    uint32_t total = SLYZ_HEADER_SIZE + const_size + code_size;
    uint8_t *img = (uint8_t *)xmalloc(total);
    memset(img, 0, SLYZ_HEADER_SIZE);

    const char *magic = "SLYZBIN";
    memcpy(img, magic, 7);
    img[7] = 0;

    put_u16(img + SLYZ_HDR_VERSION, SLYZ_VERSION);
    put_u16(img + SLYZ_HDR_FLAGS, cc->uses_gui ? SLYZ_FLAG_GUI : 0);
    put_u32(img + SLYZ_HDR_CONSTSIZE, const_size);
    put_u32(img + SLYZ_HDR_CODESIZE, code_size);
    put_u32(img + SLYZ_HDR_ENTRY, cc->entry_offset);
    put_u32(img + SLYZ_HDR_GLOBALS, (uint32_t)cc->global_count);
    put_u32(img + SLYZ_HDR_STACKHINT,
            (uint32_t)(cc->max_stack + 32 > SLYZ_MAX_STACK ? SLYZ_MAX_STACK : cc->max_stack + 32));
    put_u32(img + SLYZ_HDR_SRCSIZE, (uint32_t)cc->source_len);

    if (const_size > 0) memcpy(img + SLYZ_HEADER_SIZE, cc->consts.data, const_size);
    if (code_size > 0) memcpy(img + SLYZ_HEADER_SIZE + const_size, cc->code, code_size);

    uint32_t sum = fnv1a(img + SLYZ_HEADER_SIZE, const_size + code_size);
    put_u32(img + SLYZ_HDR_CHECKSUM, sum);

    *out = img;
    *out_size = total;
    return 1;
}
