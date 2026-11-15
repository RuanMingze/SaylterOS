/* ============================================================
 * sha256.h - 纯 C 的 SHA-256 实现（无外部依赖）
 * ============================================================ */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

/* 计算 SHA-256，输出 32 字节原始摘要 */
void sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);

/* 计算 SHA-256 并转成 64 字符小写十六进制（out 至少 65 字节） */
void sha256_hex(const uint8_t *data, uint32_t len, char out[65]);

#endif /* SHA256_H */
