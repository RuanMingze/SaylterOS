/* ============================================================
 * tls.h - TLS 1.3 客户端（仅 TLS_AES_128_GCM_SHA256 / X25519）
 * ------------------------------------------------------------
 * 用法：
 *   int rc = tls_connect(host, 443, 1);
 *   tls_write(req, len);
 *   while ((n = tls_read(buf, sizeof(buf))) > 0) ...
 *   tls_close();
 *
 * 只支持 TLS 1.3；服务器只支持 TLS 1.2 时返回
 * TLS_ERR_UNSUPPORTED，错误信息里给出原因。
 * ============================================================ */

#ifndef TLS_H
#define TLS_H

#include <stdint.h>

#define TLS_OK              0
#define TLS_ERR_IO         (-1)
#define TLS_ERR_PROTOCOL   (-2)
#define TLS_ERR_VERIFY     (-3)
#define TLS_ERR_UNSUPPORTED (-4)
#define TLS_ERR_ALERT      (-5)

int  tls_connect(const char *host, uint16_t port, int verify);
int  tls_connect_addr(const char *host, uint32_t ip, uint16_t port, int verify);
int  tls_write(const void *data, uint32_t len);
int  tls_read(void *buf, uint32_t len);
void tls_close(void);

int  tls_is_open(void);
const char *tls_error_text(void);

/* 打开握手字节级诊断（curl -v 使用）。 */
void tls_set_debug(int enabled);

#endif /* TLS_H */
