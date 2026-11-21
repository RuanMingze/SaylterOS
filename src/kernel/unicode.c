/* ============================================================
 * unicode.c - UTF-8 编解码与显示宽度
 * ------------------------------------------------------------
 * 见 unicode.h 的设计说明。这里额外注意两点：
 *   - 解码器把"需要更多字节"和"确定非法"分开返回，调用方在
 *     输入流里可以正确等待下一字节，而在缓冲区里遇到非法序列
 *     只需前进 1 字节，不会吞掉后面的合法字符。
 *   - 宽度表只覆盖现代终端会遇到的区间，未命中一律按 1 列，
 *     保证未知字符至少不会吃掉两倍空间。
 * ============================================================ */

#include "unicode.h"

int utf8_decode(const uint8_t *s, int len, uint32_t *out) {
    if (!s || !out || len <= 0) return UTF8_NEED_MORE;

    uint8_t c0 = s[0];
    if (c0 < 0x80) {
        *out = (uint32_t)c0;
        return 1;
    }

    int need;
    uint32_t cp;
    uint32_t min_cp;

    if ((c0 & 0xE0) == 0xC0) {
        need = 1;
        cp = (uint32_t)(c0 & 0x1F);
        min_cp = 0x80;
    } else if ((c0 & 0xF0) == 0xE0) {
        need = 2;
        cp = (uint32_t)(c0 & 0x0F);
        min_cp = 0x800;
    } else if ((c0 & 0xF8) == 0xF0) {
        need = 3;
        cp = (uint32_t)(c0 & 0x07);
        min_cp = 0x10000;
    } else {
        /* 0x80..0xBF 是孤立续字节，0xF8..0xFF 是 UTF-8 不允许的首字节 */
        *out = UTF8_REPLACEMENT;
        return UTF8_INVALID;
    }

    if (len < need + 1) return UTF8_NEED_MORE;

    for (int i = 1; i <= need; i++) {
        uint8_t ci = s[i];
        if ((ci & 0xC0) != 0x80) {
            *out = UTF8_REPLACEMENT;
            return UTF8_INVALID;
        }
        cp = (cp << 6) | (uint32_t)(ci & 0x3F);
    }

    /* 过长编码 / 代理区 / 超出 Unicode 范围都属于非法 */
    if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        *out = UTF8_REPLACEMENT;
        return need + 1;
    }

    *out = cp;
    return need + 1;
}

int utf8_encode(uint32_t cp, uint8_t *buf) {
    if (!buf) return 0;
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = UTF8_REPLACEMENT;

    if (cp < 0x80u) {
        buf[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800u) {
        buf[0] = (uint8_t)(0xC0u | (cp >> 6));
        buf[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        buf[0] = (uint8_t)(0xE0u | (cp >> 12));
        buf[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    buf[0] = (uint8_t)(0xF0u | (cp >> 18));
    buf[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    buf[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    buf[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4;
}

int utf32_width(uint32_t cp) {
    /* SlyzOS 私有区字形：占两个 16px 单元格（32x32）。 */
    if (cp == 0xF0001u) return 2;

    /* 控制字符与零宽字符不占列 */
    if (cp == 0 || cp == 0x200Bu || cp == 0x200Cu || cp == 0x200Du ||
        (cp >= 0x200Eu && cp <= 0x200Fu) || cp == 0xFEFFu) {
        return 0;
    }
    if (cp < 0x20u || (cp >= 0x7Fu && cp < 0xA0u)) return 0;

    /* 组合附加符：与前一字符重叠，不占列 */
    if ((cp >= 0x0300u && cp <= 0x036Fu) ||
        (cp >= 0x1AB0u && cp <= 0x1AFFu) ||
        (cp >= 0x1DC0u && cp <= 0x1DFFu) ||
        (cp >= 0x20D0u && cp <= 0x20F0u) ||
        (cp >= 0xFE00u && cp <= 0xFE0Fu) ||
        (cp >= 0xFE20u && cp <= 0xFE2Fu)) {
        return 0;
    }

    /* 东亚宽字符（Unicode East Asian Width = W / F 的实际子集） */
    if ((cp >= 0x1100u && cp <= 0x115Fu) ||          /* 韩文字母 */
        (cp >= 0x2E80u && cp <= 0x303Eu) ||          /* CJK 部首与标点 */
        (cp >= 0x3041u && cp <= 0x33FFu) ||          /* 假名 / 注音 / 兼容 */
        (cp >= 0x3400u && cp <= 0x4DBFu) ||          /* 扩展 A */
        (cp >= 0x4E00u && cp <= 0x9FFFu) ||          /* 统一表意文字 */
        (cp >= 0xA000u && cp <= 0xA4CFu) ||          /* 彝文 */
        (cp >= 0xAC00u && cp <= 0xD7A3u) ||          /* 韩文音节 */
        (cp >= 0xF900u && cp <= 0xFAFFu) ||          /* 兼容表意文字 */
        (cp >= 0xFE10u && cp <= 0xFE19u) ||          /* 竖排标点 */
        (cp >= 0xFE30u && cp <= 0xFE6Fu) ||          /* CJK 兼容形式 */
        (cp >= 0xFF00u && cp <= 0xFF60u) ||          /* 全角形式 */
        (cp >= 0xFFE0u && cp <= 0xFFE6u) ||          /* 全角符号 */
        (cp >= 0x1F300u && cp <= 0x1F64Fu) ||        /* 表情符号 */
        (cp >= 0x1F900u && cp <= 0x1F9FFu) ||
        (cp >= 0x20000u && cp <= 0x3FFFDu)) {        /* 扩展 B 及以后 */
        return 2;
    }
    return 1;
}

int utf8_width_n(const char *s, int bytes) {
    if (!s || bytes <= 0) return 0;
    int cols = 0;
    int i = 0;
    while (i < bytes && s[i]) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, bytes - i, &cp);
        if (used == UTF8_NEED_MORE || used == UTF8_INVALID) {
            /* 截断在字符中间：剩余字节不再计入显示宽度 */
            break;
        }
        cols += utf32_width(cp);
        i += used;
    }
    return cols;
}

int utf8_width(const char *s) {
    if (!s) return 0;
    int bytes = 0;
    while (s[bytes]) bytes++;
    return utf8_width_n(s, bytes);
}

int utf8_byte_offset(const char *s, int n) {
    if (!s) return 0;
    int i = 0;
    int count = 0;
    while (s[i] && count < n) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, 4, &cp);
        if (used == UTF8_NEED_MORE || used == UTF8_INVALID) used = 1;
        i += used;
        count++;
    }
    return i;
}

int utf8_col_offset(const char *s, int n) {
    if (!s) return 0;
    int i = 0;
    int cols = 0;
    int count = 0;
    while (s[i] && count < n) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, 4, &cp);
        if (used == UTF8_NEED_MORE || used == UTF8_INVALID) {
            used = 1;
            cp = UTF8_REPLACEMENT;
        }
        cols += utf32_width(cp);
        i += used;
        count++;
    }
    return cols;
}

int utf8_char_index(const char *s, int byte_off) {
    if (!s || byte_off <= 0) return 0;
    int i = 0;
    int count = 0;
    while (s[i] && i < byte_off) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, 4, &cp);
        if (used == UTF8_NEED_MORE || used == UTF8_INVALID) used = 1;
        i += used;
        count++;
    }
    return count;
}

int utf8_prev_char(const char *s, int byte_off) {
    if (!s || byte_off <= 0) return 0;
    int i = byte_off - 1;
    while (i > 0 && ((uint8_t)s[i] & 0xC0u) == 0x80u) {
        i--;
    }
    return i;
}

int utf8_next_char(const char *s, int byte_off) {
    if (!s || byte_off < 0) return 0;
    if (!s[byte_off]) return byte_off;
    uint32_t cp = 0;
    int used = utf8_decode((const uint8_t *)s + byte_off, 4, &cp);
    if (used == UTF8_NEED_MORE || used == UTF8_INVALID) used = 1;
    return byte_off + used;
}

int utf8_truncate(const char *s, int max_bytes) {
    if (!s || max_bytes <= 0) return 0;
    int i = 0;
    int last_ok = 0;
    while (s[i] && i < max_bytes) {
        uint32_t cp = 0;
        int used = utf8_decode((const uint8_t *)s + i, max_bytes - i, &cp);
        if (used == UTF8_NEED_MORE) {
            /* 剩余空间放不下这个字符，截到上一个完整边界 */
            return last_ok;
        }
        if (used == UTF8_INVALID) {
            i += 1;
        } else {
            i += used;
        }
        if (i <= max_bytes) last_ok = i;
    }
    return last_ok;
}
