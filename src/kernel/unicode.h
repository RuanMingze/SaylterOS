/* ============================================================
 * unicode.h - UTF-8 编解码与显示宽度
 * ------------------------------------------------------------
 * 内核没有 libc，也没有 locale。这里只做两件必须做对的事：
 *   1. 严格解码 UTF-8（拒绝过长编码、代理区、越界码位），
 *      因为 shell 缓冲区、文件名、编辑器内容全部按字节存，
 *      一旦把半个多字节序列当成字符处理，后续所有编辑都会错位。
 *   2. 给出东亚宽字符的显示列数，终端按"列"排版，
 *      16px 的 ASCII 与 32px 的汉字必须区分开。
 * ============================================================ */

#ifndef UNICODE_H
#define UNICODE_H

#include <stdint.h>

#define UTF8_REPLACEMENT   0xFFFDu
#define UTF8_NEED_MORE     (-2)     /* 序列尚未接收完整 */
#define UTF8_INVALID       (-1)     /* 非法序列 */

/* 解码一个 UTF-8 字符。
 * s/len：输入字节串与长度；out：写入码位（非法时写 U+FFFD）。
 * 返回消耗的字节数（1..4），UTF8_NEED_MORE 表示需要更多字节，
 * UTF8_INVALID 表示确定的非法序列（调用方应跳过 s[0] 继续）。 */
int utf8_decode(const uint8_t *s, int len, uint32_t *out);

/* 把码位编码为 UTF-8，返回字节数（1..4），buf 至少 4 字节。
 * 非法码位编码为 U+FFFD。 */
int utf8_encode(uint32_t cp, uint8_t *buf);

/* 码位的显示列数：0（组合符/控制符）、1（半角）、2（全角）。
 * 未检索到的零宽字符表很小，只覆盖常见的组合附加符。 */
int utf32_width(uint32_t cp);

/* 字节串的显示列数。非法字节按 1 列计。 */
int utf8_width(const char *s);
int utf8_width_n(const char *s, int bytes);

/* 第 n 个字符的字节偏移（n 超过字符数时返回串长）。 */
int utf8_byte_offset(const char *s, int n);

/* 第 n 个字符的显示列偏移。 */
int utf8_col_offset(const char *s, int n);

/* 字节偏移所在的字符序号（偏移落在多字节序列中间时取所属字符）。 */
int utf8_char_index(const char *s, int byte_off);

/* 字节偏移的前一个 / 后一个字符边界，用于退格与右删。
 * 越界时返回原偏移。 */
int utf8_prev_char(const char *s, int byte_off);
int utf8_next_char(const char *s, int byte_off);

/* 尾部截断：把 s 截到能容纳最多 max_bytes 的完整字符边界，
 * 返回新的字节长度。用于 DirEntry.name[52] 这类定长字段。 */
int utf8_truncate(const char *s, int max_bytes);

#endif /* UNICODE_H */
