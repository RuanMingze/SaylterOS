/* ============================================================
 * slyzc_lex.c - SLYZ 词法分析
 * ------------------------------------------------------------
 * 输入是 UTF-8 字节流。标识符与关键字只接受 ASCII，字符串
 * 字面量内部原样保留 UTF-8 字节，因此中文界面文案不需要额外
 * 编码转换。
 *
 * 支持：
 *   行注释（双斜线）、块注释（斜线加星号，星号加斜线闭合）、
 *   行首 '#' 注释
 *   十进制 / 0x 十六进制 / 0b 二进制整数，允许下划线分组
 *   字符串转义 \n \t \r \\ \" \0 \xNN \u{XXXX}
 *   两字符运算符最长匹配（== != <= >= && || += -= *= /= %= ++ -- ..）
 * ============================================================ */

#include "slyzc.h"
#include <stdarg.h>

static const char *g_src = 0;
static int         g_len = 0;
static int         g_pos = 0;
static int         g_line = 1;
static int         g_col = 1;

static Token       g_ahead;
static int         g_has_ahead = 0;

/* ---------- 诊断 ---------- */

void diag_error(int line, int col, const char *fmt, ...) {
    va_list ap;
    char msg[SLYZ_DIAG_MSG_MAX];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    slyz_diag_record(0, line, col, msg);
    g_cc.error_count++;
}

void diag_warn(int line, int col, const char *fmt, ...) {
    va_list ap;
    char msg[SLYZ_DIAG_MSG_MAX];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    slyz_diag_record(1, line, col, msg);
}

/* ---------- 基础字符分类 ---------- */

static int is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_ident_char(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_digit(int c) {
    return c >= '0' && c <= '9';
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---------- 位置推进 ---------- */

static int peek_char(int ahead) {
    int p = g_pos + ahead;
    if (p < 0 || p >= g_len) return -1;
    return (unsigned char)g_src[p];
}

static void advance_char(void) {
    if (g_pos >= g_len) return;
    if (g_src[g_pos] == '\n') {
        g_line++;
        g_col = 1;
    } else {
        g_col++;
    }
    g_pos++;
}

/* ---------- 注释与空白 ---------- */

static void skip_space_and_comments(void) {
    for (;;) {
        int c = peek_char(0);

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance_char();
            continue;
        }

        if (c == '/' && peek_char(1) == '/') {
            while (peek_char(0) != -1 && peek_char(0) != '\n') advance_char();
            continue;
        }

        if (c == '/' && peek_char(1) == '*') {
            int line = g_line;
            int col = g_col;
            advance_char();
            advance_char();
            for (;;) {
                if (peek_char(0) == -1) {
                    diag_error(line, col, "unterminated block comment");
                    return;
                }
                if (peek_char(0) == '*' && peek_char(1) == '/') {
                    advance_char();
                    advance_char();
                    break;
                }
                advance_char();
            }
            continue;
        }

        if (c == '#' && g_col == 1) {
            while (peek_char(0) != -1 && peek_char(0) != '\n') advance_char();
            continue;
        }

        break;
    }
}

/* ---------- 字符串字面量 ---------- */

static void append_utf8(char **buf, int *len, int *cap, uint32_t cp) {
    if (*len + 4 >= *cap) {
        int next = *cap * 2 + 32;
        char *nb = (char *)xmalloc((size_t)next);
        memcpy(nb, *buf, (size_t)*len);
        free(*buf);
        *buf = nb;
        *cap = next;
    }

    if (cp < 0x80) {
        (*buf)[(*len)++] = (char)cp;
    } else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        (*buf)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        (*buf)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static int read_string(Token *t) {
    int cap = 32;
    int len = 0;
    char *buf = (char *)xmalloc((size_t)cap);

    advance_char();   /* 开引号 */

    for (;;) {
        int c = peek_char(0);
        if (c == -1 || c == '\n') {
            diag_error(t->line, t->col, "unterminated string literal");
            free(buf);
            t->sval = 0;
            t->slen = 0;
            return 0;
        }

        if (c == '"') {
            advance_char();
            break;
        }

        if (c != '\\') {
            if (len + 1 >= cap) {
                int next = cap * 2 + 32;
                char *nb = (char *)xmalloc((size_t)next);
                memcpy(nb, buf, (size_t)len);
                free(buf);
                buf = nb;
                cap = next;
            }
            buf[len++] = (char)c;
            advance_char();
            continue;
        }

        /* 转义序列 */
        advance_char();
        int e = peek_char(0);
        if (e == -1) {
            diag_error(t->line, t->col, "unterminated escape sequence");
            free(buf);
            t->sval = 0;
            t->slen = 0;
            return 0;
        }

        advance_char();

        if (e == 'n') {
            buf[len++] = '\n';
        } else if (e == 't') {
            buf[len++] = '\t';
        } else if (e == 'r') {
            buf[len++] = '\r';
        } else if (e == '0') {
            buf[len++] = '\0';
        } else if (e == '\\') {
            buf[len++] = '\\';
        } else if (e == '"') {
            buf[len++] = '"';
        } else if (e == '\'') {
            buf[len++] = '\'';
        } else if (e == 'x') {
            int hi = hex_value(peek_char(0));
            int lo = hex_value(peek_char(1));
            if (hi < 0 || lo < 0) {
                diag_error(t->line, t->col, "\\x needs two hex digits");
                free(buf);
                t->sval = 0;
                t->slen = 0;
                return 0;
            }
            advance_char();
            advance_char();
            buf[len++] = (char)((hi << 4) | lo);
        } else if (e == 'u') {
            if (peek_char(0) != '{') {
                diag_error(t->line, t->col, "\\u needs the form \\u{1F600}");
                free(buf);
                t->sval = 0;
                t->slen = 0;
                return 0;
            }
            advance_char();
            uint32_t cp = 0;
            int digits = 0;
            for (;;) {
                int h = hex_value(peek_char(0));
                if (h < 0) break;
                cp = cp * 16u + (uint32_t)h;
                digits++;
                advance_char();
                if (cp > 0x10FFFFu) {
                    diag_error(t->line, t->col, "unicode escape out of range");
                    free(buf);
                    t->sval = 0;
                    t->slen = 0;
                    return 0;
                }
            }
            if (digits == 0 || peek_char(0) != '}') {
                diag_error(t->line, t->col, "\\u{...} needs 1..6 hex digits then '}'");
                free(buf);
                t->sval = 0;
                t->slen = 0;
                return 0;
            }
            advance_char();
            append_utf8(&buf, &len, &cap, cp);
        } else {
            diag_error(t->line, t->col, "unknown escape sequence \\%c", (char)e);
            free(buf);
            t->sval = 0;
            t->slen = 0;
            return 0;
        }
    }

    if (len + 1 > cap) {
        int next = len + 1;
        char *nb = (char *)xmalloc((size_t)next);
        memcpy(nb, buf, (size_t)len);
        free(buf);
        buf = nb;
    }
    buf[len] = '\0';

    t->sval = buf;
    t->slen = len;
    return 1;
}

/* ---------- 数字字面量 ---------- */

static void read_number(Token *t) {
    int base = 10;
    uint64_t value = 0;
    int digits = 0;

    if (peek_char(0) == '0' && (peek_char(1) == 'x' || peek_char(1) == 'X')) {
        base = 16;
        advance_char();
        advance_char();
    } else if (peek_char(0) == '0' && (peek_char(1) == 'b' || peek_char(1) == 'B')) {
        base = 2;
        advance_char();
        advance_char();
    }

    for (;;) {
        int c = peek_char(0);

        if (c == '_') {
            advance_char();
            continue;
        }

        int d = -1;
        if (base == 16) d = hex_value(c);
        else if (base == 2) {
            if (c == '0' || c == '1') d = c - '0';
        } else {
            if (is_digit(c)) d = c - '0';
        }

        if (d < 0) break;

        if (d >= base) {
            diag_error(t->line, t->col, "digit '%c' is not valid in base %d", (char)c, base);
            return;
        }

        if (value > (uint64_t)0x7FFFFFFFFFFFFFFFull / (uint64_t)base) {
            diag_error(t->line, t->col, "integer literal overflow (max 9223372036854775807)");
            return;
        }

        value = value * (uint64_t)base + (uint64_t)d;
        digits++;
        advance_char();
    }

    if (digits == 0) {
        diag_error(t->line, t->col, "malformed integer literal");
        return;
    }

    t->ival = (int64_t)value;
}

/* ---------- 关键字 ---------- */

static const char *g_keywords[] = {
    "let", "var", "fn", "if", "else", "while", "for", "in",
    "return", "break", "continue", "true", "false", "nil",
    "and", "or", "not", 0
};

static int is_keyword(const char *s) {
    for (int i = 0; g_keywords[i]; i++) {
        if (strcmp(g_keywords[i], s) == 0) return 1;
    }
    return 0;
}

/* ---------- 多字符运算符 ---------- */

static const char *g_puncts[] = {
    "==", "!=", "<=", ">=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "++", "--", "..",
    "+", "-", "*", "/", "%", "=", "<", ">", "!", "(", ")", "{", "}",
    "[", "]", ",", ".", ":", ";", 0
};

/* ---------- 主扫描 ---------- */

static Token lex_scan(void) {
    Token t;
    memset(&t, 0, sizeof(t));

    skip_space_and_comments();

    t.line = g_line;
    t.col = g_col;

    int c = peek_char(0);
    if (c == -1) {
        t.kind = T_EOF;
        return t;
    }

    if (is_ident_start(c)) {
        int n = 0;
        while (is_ident_char(peek_char(0))) {
            if (n < NAME_MAX_LEN - 1) t.text[n++] = (char)peek_char(0);
            advance_char();
        }
        t.text[n] = '\0';
        t.kind = is_keyword(t.text) ? T_KW : T_IDENT;
        return t;
    }

    if (is_digit(c)) {
        t.kind = T_INT;
        read_number(&t);
        return t;
    }

    if (c == '"') {
        t.kind = T_STR;
        if (!read_string(&t)) t.kind = T_STR;
        return t;
    }

    if (c == '\'') {
        /* 单字符字面量 'a'，作为整数处理，便于写按键判断 */
        advance_char();
        int v = peek_char(0);
        if (v == -1 || v == '\n') {
            diag_error(t.line, t.col, "unterminated character literal");
            t.kind = T_INT;
            return t;
        }
        if (v == '\\') {
            advance_char();
            int e = peek_char(0);
            if (e == 'n') v = '\n';
            else if (e == 't') v = '\t';
            else if (e == 'r') v = '\r';
            else if (e == '0') v = '\0';
            else if (e == '\\') v = '\\';
            else if (e == '\'') v = '\'';
            else {
                diag_error(t.line, t.col, "unknown character escape");
            }
        }
        advance_char();
        if (peek_char(0) != '\'') {
            diag_error(t.line, t.col, "character literal must contain exactly one byte");
        } else {
            advance_char();
        }
        t.kind = T_INT;
        t.ival = (int64_t)(unsigned char)v;
        return t;
    }

    for (int i = 0; g_puncts[i]; i++) {
        int n = (int)strlen(g_puncts[i]);
        int match = 1;
        for (int k = 0; k < n; k++) {
            if (peek_char(k) != (unsigned char)g_puncts[i][k]) {
                match = 0;
                break;
            }
        }
        if (match) {
            for (int k = 0; k < n; k++) advance_char();
            t.kind = T_PUNCT;
            memcpy(t.text, g_puncts[i], (size_t)n + 1);
            return t;
        }
    }

    diag_error(t.line, t.col, "unexpected character 0x%02X", (unsigned char)c);
    advance_char();
    t.kind = T_PUNCT;
    t.text[0] = '?';
    t.text[1] = '\0';
    return t;
}

void lex_init(const char *source, int len, const char *path) {
    g_src = source;
    g_len = len;
    g_pos = 0;
    g_line = 1;
    g_col = 1;
    g_has_ahead = 0;
    memset(&g_ahead, 0, sizeof(g_ahead));
    (void)path;
}

Token lex_peek(void) {
    if (!g_has_ahead) {
        g_ahead = lex_scan();
        g_has_ahead = 1;
    }
    return g_ahead;
}

void lex_consume(void) {
    if (!g_has_ahead) {
        (void)lex_scan();
        return;
    }
    g_has_ahead = 0;
}

Token lex_next(void) {
    Token t;
    if (g_has_ahead) {
        t = g_ahead;
        g_has_ahead = 0;
        return t;
    }
    return lex_scan();
}

int tok_is(const Token *t, const char *punct) {
    return t->kind == T_PUNCT && strcmp(t->text, punct) == 0;
}

int tok_is_kw(const Token *t, const char *kw) {
    return t->kind == T_KW && strcmp(t->text, kw) == 0;
}
