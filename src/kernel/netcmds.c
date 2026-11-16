/* ============================================================
 * netcmds.c - 网络命令：ip / curl / ping / nslookup
 * ------------------------------------------------------------
 * curl 是一个完整的 HTTP/1.1 客户端：
 *   - 解析 http://host[:port]/path
 *   - DNS 解析（也接受点分十进制字面量）
 *   - 自定义 User-Agent（-A，默认 SaylterOS/0.3-Beta1 (x86_64)）
 *   - 支持 -i(输出响应头) -I(HEAD) -v(过程输出) -s(静默) -m(超时)
 *   - 处理 Content-Length 与 Transfer-Encoding: chunked
 *   - 响应体边收边显示，不整包缓存，避免大页面耗尽内核堆
 *
 * 所有屏幕文本均为纯 ASCII；大块正文走 tty_putchar_vga，
 * 不写 COM1 —— 9600bps 下把每个字节都回显到串口会让下载慢几十倍。
 * ============================================================ */

#include <stdint.h>
#include "tty.h"
#include "net.h"

#define NC_LINE_MAX 512

/* ============================================================
 * 字符串工具（内核没有 libc）
 * ============================================================ */

static int nc_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int nc_strcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int nc_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int nc_strncasecmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        int ca = nc_lower((unsigned char)a[i]);
        int cb = nc_lower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

static int nc_contains_case(const char *haystack, const char *needle) {
    int nl = nc_strlen(needle);
    if (nl == 0) return 1;
    for (int i = 0; haystack[i]; i++) {
        if (nc_strncasecmp(haystack + i, needle, nl) == 0) return 1;
    }
    return 0;
}

static void nc_copy(char *dst, const char *src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int nc_append(char *dst, int *pos, int cap, const char *src) {
    while (*src) {
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = '\0';
    return 1;
}

static int nc_parse_u32(const char *s, uint32_t *out) {
    uint32_t value = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10u + (uint32_t)(*s - '0');
        digits++;
        s++;
    }
    if (digits == 0) return 0;
    *out = value;
    return 1;
}

static int nc_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 把一行拆成 argv，支持双引号包裹带空格的参数（自定义 UA 常用） */
static int nc_tokenize(char *line, char *argv[], int max) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') {
                *p = '\0';
                p++;
            }
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) {
                *p = '\0';
                p++;
            }
        }
    }
    return argc;
}

/* ============================================================
 * 输出辅助
 * ============================================================ */

static void nc_out(const char *s) {
    while (*s) {
        tty_putchar_vga(*s++);
    }
}

static void nc_out_line(const char *s) {
    nc_out(s);
    tty_putchar_vga('\n');
}

struct BodySink {
    uint32_t bytes;
    int      last_char;
};

static void body_write(struct BodySink *sink, const uint8_t *data, uint32_t len) {
    tty_set_color(0x0F);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        /* CRLF -> LF；丢弃 NUL 与其它控制字符，避免终端状态被破坏 */
        if (c == '\r' || c == '\0') continue;
        if (c < 0x20 && c != '\n' && c != '\t') continue;
        tty_putchar_vga((char)c);
        sink->last_char = (int)c;
    }
    sink->bytes += len;
}

/* ============================================================
 * HTTP 流式读取
 * ============================================================ */

struct HttpStream {
    uint8_t  buf[1024];
    int      pos;
    int      len;
    int      eof;
    int      error;
    uint32_t timeout_ms;
};

static int hs_fill(struct HttpStream *s) {
    s->pos = 0;
    s->len = 0;

    int n = net_tcp_recv(s->buf, (uint32_t)sizeof(s->buf), s->timeout_ms);
    if (n > 0) {
        s->len = n;
        return n;
    }
    if (n == 0) {
        s->eof = 1;
        return 0;
    }
    s->error = 1;
    return -1;
}

static int hs_getc(struct HttpStream *s) {
    if (s->pos >= s->len) {
        if (s->eof) return -1;
        if (hs_fill(s) <= 0) return -1;
    }
    return s->buf[s->pos++];
}

static int hs_read(struct HttpStream *s, uint8_t *dst, int n) {
    int got = 0;

    while (got < n) {
        if (s->pos >= s->len) {
            if (s->eof) break;
            if (hs_fill(s) <= 0) break;
        }
        int avail = s->len - s->pos;
        int take = n - got;
        if (take > avail) take = avail;

        for (int i = 0; i < take; i++) {
            dst[got + i] = s->buf[s->pos + i];
        }
        s->pos += take;
        got += take;
    }

    if (got == 0 && s->error) return -1;
    return got;
}

static int hs_readline(struct HttpStream *s, char *out, int cap) {
    int n = 0;

    /* printk 的格式化缓冲只有 512 字节且不做边界检查，
     * 这里把单行硬限制在 380 字符，保证 "curl: malformed ...: %s" 这类
     * 拼接触发时也不会溢出；超出部分继续消费到换行，避免错位。 */
    int limit = cap - 1;
    if (limit > 380) limit = 380;

    for (;;) {
        int c = hs_getc(s);
        if (c < 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < limit) out[n++] = (char)c;
    }

    out[n] = '\0';
    if (n == 0 && (s->eof || s->error)) {
        return -1;
    }
    return n;
}

/* ============================================================
 * URL 解析
 * ============================================================ */

struct HttpUrl {
    char     host[128];
    uint16_t port;
    char     path[512];
};

static int parse_http_url(const char *url, struct HttpUrl *out) {
    if (nc_strncasecmp(url, "http://", 7) != 0) return 0;

    const char *p = url + 7;
    const char *host_start = p;

    while (*p && *p != '/' && *p != ':') p++;
    int host_len = (int)(p - host_start);
    if (host_len <= 0 || host_len >= (int)sizeof(out->host)) return 0;

    for (int i = 0; i < host_len; i++) {
        out->host[i] = host_start[i];
    }
    out->host[host_len] = '\0';

    out->port = 80;

    if (*p == ':') {
        p++;
        uint32_t port = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + (uint32_t)(*p - '0');
            digits++;
            p++;
        }
        if (digits == 0 || port == 0 || port > 65535u) return 0;
        out->port = (uint16_t)port;
    }

    if (*p == '/') {
        nc_copy(out->path, p, (int)sizeof(out->path));
    } else if (*p == '\0') {
        nc_copy(out->path, "/", (int)sizeof(out->path));
    } else {
        return 0;
    }

    return 1;
}

/* ============================================================
 * ip / ifconfig
 * ============================================================ */

static void nc_hex2(char out[3], uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    out[0] = digits[(value >> 4) & 0x0F];
    out[1] = digits[value & 0x0F];
    out[2] = '\0';
}

static void print_ip_config(void) {
    char mac[18];
    char ip[16];
    char mask[16];
    char gw[16];
    char dns[16];
    char bus_hex[3];
    char slot_hex[3];

    net_format_mac(net_info()->mac, mac);
    net_format_ipv4(net_info()->ip, ip);
    net_format_ipv4(net_info()->netmask, mask);
    net_format_ipv4(net_info()->gateway, gw);
    net_format_ipv4(net_info()->dns, dns);
    nc_hex2(bus_hex, net_info()->bus);
    nc_hex2(slot_hex, net_info()->slot);

    printk("eth0: link %s, %u Mbps, Intel E1000 (%x:%x)\n",
           net_info()->link_up ? "up" : "down",
           (unsigned int)net_info()->speed_mbps,
           (unsigned int)net_info()->vendor_id,
           (unsigned int)net_info()->device_id);
    printk("      mac     %s\n", mac);
    printk("      inet    %s  netmask %s\n", ip, mask);
    printk("      gateway %s\n", gw);
    printk("      dns     %s\n", dns);
    printk("      pci     %s:%s.%u  mmio 0x%x\n",
           bus_hex, slot_hex,
           (unsigned int)net_info()->func,
           (unsigned int)net_info()->mmio_base);
    printk("      rx      %u packets / %u bytes\n",
           (unsigned int)net_info()->rx_packets,
           (unsigned int)net_info()->rx_bytes);
    printk("      tx      %u packets / %u bytes\n",
           (unsigned int)net_info()->tx_packets,
           (unsigned int)net_info()->tx_bytes);
    printk("      errors  rx %u  tx %u\n",
           (unsigned int)net_info()->rx_errors,
           (unsigned int)net_info()->tx_errors);
    printk("      arp     req %u  rep %u  miss %u\n",
           (unsigned int)net_info()->arp_requests,
           (unsigned int)net_info()->arp_replies,
           (unsigned int)net_info()->arp_misses);
}

void netcmd_ip(const char *args) {
    int rc = net_init();
    if (rc == -1) {
        printk("ip: no supported Intel E1000 adapter found on the PCI bus\n");
        return;
    }
    if (rc == -2) {
        printk("ip: adapter found but MMIO/BAR initialization failed\n");
        return;
    }

    char work[NC_LINE_MAX];
    nc_copy(work, args ? args : "", (int)sizeof(work));

    char *argv[8];
    int argc = nc_tokenize(work, argv, 8);

    if (argc >= 1 && nc_strcmp(argv[0], "set") == 0) {
        if (argc < 3) {
            printk("usage: ip set <address> <netmask> [gateway] [dns]\n");
            return;
        }

        uint32_t ip = 0;
        uint32_t mask = 0;
        uint32_t gw = 0;
        uint32_t dns = 0;

        if (!net_parse_ipv4(argv[1], &ip) || !net_parse_ipv4(argv[2], &mask)) {
            printk("ip: invalid IPv4 address or netmask\n");
            return;
        }
        if (argc >= 4) {
            if (!net_parse_ipv4(argv[3], &gw)) {
                printk("ip: invalid gateway address\n");
                return;
            }
        }
        if (argc >= 5) {
            if (!net_parse_ipv4(argv[4], &dns)) {
                printk("ip: invalid DNS address\n");
                return;
            }
        }

        net_set_config(ip, mask, gw, dns);
        printk("ip: address configuration updated\n");
        print_ip_config();
        return;
    }

    printk("eth0: static IPv4 configuration (use 'ip set' to change)\n");
    print_ip_config();
}

/* ============================================================
 * curl
 * ============================================================ */

static void curl_usage(void) {
    printk("usage: curl [-i] [-I] [-v] [-s] [-A <agent>] [-m <seconds>] <url>\n");
    printk("       url must start with http://, e.g. http://10.0.2.2:8000/index.html\n");
}

static void parse_status_code(const char *line, int *code) {
    *code = 0;
    int i = 0;
    while (line[i] && line[i] != ' ') i++;
    while (line[i] == ' ') i++;

    int value = 0;
    int digits = 0;
    while (line[i] >= '0' && line[i] <= '9' && digits < 3) {
        value = value * 10 + (line[i] - '0');
        digits++;
        i++;
    }
    if (digits == 3) *code = value;
}

static int header_value(const char *line, const char *name, const char **value) {
    int i = 0;
    while (name[i]) {
        if (nc_lower((unsigned char)line[i]) != nc_lower((unsigned char)name[i])) {
            return 0;
        }
        i++;
    }
    if (line[i] != ':') return 0;
    i++;
    while (line[i] == ' ' || line[i] == '\t') i++;
    *value = line + i;
    return 1;
}

static int http_read_chunked(struct HttpStream *hs, struct BodySink *sink) {
    char line[128];

    for (;;) {
        if (hs_readline(hs, line, (int)sizeof(line)) < 0) return -1;

        uint32_t size = 0;
        int digits = 0;
        for (int i = 0; line[i]; i++) {
            int v = nc_hex_value(line[i]);
            if (v < 0) break;
            if (size > 0x0FFFFFFFu) return -1;
            size = (size << 4) | (uint32_t)v;
            digits++;
        }
        if (digits == 0) return -1;

        if (size == 0) {
            /* 最后一个 chunk：吞掉可选的 trailer 直到空行 */
            while (hs_readline(hs, line, (int)sizeof(line)) > 0) {
            }
            return 0;
        }

        uint32_t remaining = size;
        while (remaining > 0) {
            uint8_t tmp[512];
            uint32_t want = (remaining > sizeof(tmp)) ? (uint32_t)sizeof(tmp) : remaining;
            int n = hs_read(hs, tmp, (int)want);
            if (n <= 0) return -1;
            body_write(sink, tmp, (uint32_t)n);
            remaining -= (uint32_t)n;
        }

        /* chunk 数据之后必须跟一个 CRLF */
        if (hs_readline(hs, line, (int)sizeof(line)) < 0) return -1;
    }
}

void netcmd_curl(const char *args) {
    char work[NC_LINE_MAX];
    nc_copy(work, args ? args : "", (int)sizeof(work));

    char *argv[16];
    int argc = nc_tokenize(work, argv, 16);

    int show_headers = 0;
    int head_only = 0;
    int verbose = 0;
    int silent = 0;
    const char *user_agent = "SaylterOS/0.3-Beta1 (x86_64)";
    uint32_t timeout_ms = 15000;
    const char *url = 0;

    for (int i = 0; i < argc; i++) {
        if (nc_strcmp(argv[i], "-i") == 0) {
            show_headers = 1;
        } else if (nc_strcmp(argv[i], "-I") == 0) {
            head_only = 1;
            show_headers = 1;
        } else if (nc_strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (nc_strcmp(argv[i], "-s") == 0) {
            silent = 1;
        } else if (nc_strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            user_agent = argv[++i];
        } else if (nc_strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            uint32_t seconds = 0;
            if (!nc_parse_u32(argv[++i], &seconds) || seconds == 0) {
                printk("curl: invalid timeout value\n");
                return;
            }
            timeout_ms = seconds * 1000u;
        } else {
            url = argv[i];
        }
    }

    if (!url) {
        curl_usage();
        return;
    }

    struct HttpUrl parsed;
    if (!parse_http_url(url, &parsed)) {
        printk("curl: only http:// URLs are supported (https is not implemented)\n");
        curl_usage();
        return;
    }

    uint32_t ip = 0;
    if (!net_dns_resolve(parsed.host, &ip, 5000u)) {
        printk("curl: could not resolve host: %s\n", parsed.host);
        return;
    }

    char ip_text[16];
    net_format_ipv4(ip, ip_text);

    if (verbose) {
        printk("*   Trying %s:%u...\n", ip_text, (unsigned int)parsed.port);
    }

    uint32_t t0 = net_now_ms();
    if (net_tcp_connect(ip, parsed.port, 5000u) != 0) {
        printk("curl: connect to %s:%u failed (timeout or refused)\n",
               ip_text, (unsigned int)parsed.port);
        return;
    }

    if (verbose) {
        printk("* Connected to %s (%s) port %u\n",
               parsed.host, ip_text, (unsigned int)parsed.port);
    }

    char request[1024];
    int pos = 0;
    request[0] = '\0';

    nc_append(request, &pos, (int)sizeof(request),
              head_only ? "HEAD " : "GET ");
    nc_append(request, &pos, (int)sizeof(request), parsed.path);
    nc_append(request, &pos, (int)sizeof(request), " HTTP/1.1\r\nHost: ");
    nc_append(request, &pos, (int)sizeof(request), parsed.host);
    if (parsed.port != 80) {
        char portbuf[8];
        uint32_t v = parsed.port;
        int n = 0;
        char tmp[8];
        if (v == 0) tmp[n++] = '0';
        while (v > 0) {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        int k = 0;
        portbuf[k++] = ':';
        while (n > 0) portbuf[k++] = tmp[--n];
        portbuf[k] = '\0';
        nc_append(request, &pos, (int)sizeof(request), portbuf);
    }
    nc_append(request, &pos, (int)sizeof(request), "\r\nUser-Agent: ");
    nc_append(request, &pos, (int)sizeof(request), user_agent);
    nc_append(request, &pos, (int)sizeof(request), "\r\nAccept: */*\r\n");
    nc_append(request, &pos, (int)sizeof(request), "Connection: close\r\n\r\n");

    if (pos >= (int)sizeof(request) - 1) {
        printk("curl: request headers exceed the %u byte buffer\n",
               (unsigned int)sizeof(request));
        net_tcp_close();
        return;
    }

    if (verbose) {
        int start = 0;
        while (request[start]) {
            int end = start;
            while (request[end] && request[end] != '\n') end++;
            int count = end - start;
            if (count > 0 && request[start + count - 1] == '\r') count--;
            if (count > 0) {
                char line[400];
                if (count > (int)sizeof(line) - 1) count = (int)sizeof(line) - 1;
                for (int i = 0; i < count; i++) line[i] = request[start + i];
                line[count] = '\0';
                printk("> %s\n", line);
            }
            start = end + 1;
        }
    }

    if (net_tcp_send(request, (uint32_t)pos, 10000u) != pos) {
        printk("curl: failed to send HTTP request\n");
        net_tcp_close();
        return;
    }

    struct HttpStream hs;
    hs.pos = 0;
    hs.len = 0;
    hs.eof = 0;
    hs.error = 0;
    hs.timeout_ms = timeout_ms;

    char line[NC_LINE_MAX];
    if (hs_readline(&hs, line, (int)sizeof(line)) < 0) {
        printk("curl: no response from server\n");
        net_tcp_close();
        return;
    }

    int status_code = 0;
    parse_status_code(line, &status_code);
    if (status_code == 0) {
        printk("curl: malformed HTTP response: %s\n", line);
        net_tcp_close();
        return;
    }

    if (show_headers) {
        tty_set_color(0x0B);
        nc_out_line(line);
        tty_set_color(0x0F);
    }

    int chunked = 0;
    int have_length = 0;
    uint32_t content_length = 0;

    for (;;) {
        int n = hs_readline(&hs, line, (int)sizeof(line));
        if (n < 0) break;
        if (n == 0) break;   /* 空行表示头部结束 */

        if (show_headers) {
            tty_set_color(0x0B);
            nc_out_line(line);
            tty_set_color(0x0F);
        }

        const char *value = 0;
        if (header_value(line, "Content-Length", &value)) {
            uint32_t v = 0;
            if (nc_parse_u32(value, &v)) {
                content_length = v;
                have_length = 1;
            }
        } else if (header_value(line, "Transfer-Encoding", &value)) {
            if (nc_contains_case(value, "chunked")) chunked = 1;
        }
    }

    if (!silent && verbose) {
        printk("* HTTP status %d\n", status_code);
    }

    struct BodySink sink;
    sink.bytes = 0;
    sink.last_char = '\n';

    int body_allowed = !head_only && status_code != 204 && status_code != 304 &&
                       !(status_code >= 100 && status_code < 200);

    if (body_allowed) {
        if (chunked) {
            if (http_read_chunked(&hs, &sink) < 0 && !silent) {
                printk("\ncurl: chunked stream ended unexpectedly\n");
            }
        } else if (have_length) {
            uint32_t remaining = content_length;
            while (remaining > 0) {
                uint8_t tmp[512];
                uint32_t want = (remaining > sizeof(tmp)) ? (uint32_t)sizeof(tmp) : remaining;
                int n = hs_read(&hs, tmp, (int)want);
                if (n <= 0) {
                    if (!silent) {
                        printk("\ncurl: connection closed with %u bytes left\n",
                               (unsigned int)remaining);
                    }
                    break;
                }
                body_write(&sink, tmp, (uint32_t)n);
                remaining -= (uint32_t)n;
            }
        } else {
            for (;;) {
                uint8_t tmp[512];
                int n = hs_read(&hs, tmp, (int)sizeof(tmp));
                if (n <= 0) break;
                body_write(&sink, tmp, (uint32_t)n);
            }
        }
    }

    if (sink.last_char != '\n') {
        tty_putchar_vga('\n');
    }

    net_tcp_close();

    if (!silent && verbose) {
        printk("* Received %u bytes in %u ms\n",
               (unsigned int)sink.bytes,
               (unsigned int)(net_now_ms() - t0));
    }
}

/* ============================================================
 * ping
 * ============================================================ */

void netcmd_ping(const char *args) {
    char work[NC_LINE_MAX];
    nc_copy(work, args ? args : "", (int)sizeof(work));

    char *argv[4];
    int argc = nc_tokenize(work, argv, 4);

    if (argc < 1) {
        printk("usage: ping <host|address> [count]\n");
        return;
    }

    uint32_t count = 4;
    if (argc >= 2) {
        if (!nc_parse_u32(argv[1], &count) || count == 0 || count > 100u) {
            printk("ping: count must be 1..100\n");
            return;
        }
    }

    if (net_init() != 0) {
        printk("ping: network adapter not available\n");
        return;
    }

    uint32_t ip = 0;
    if (!net_dns_resolve(argv[0], &ip, 5000u)) {
        printk("ping: cannot resolve %s\n", argv[0]);
        return;
    }

    char ip_text[16];
    net_format_ipv4(ip, ip_text);
    printk("PING %s (%s) 32 bytes of data\n", argv[0], ip_text);

    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t total_ms = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t rtt = 0;
        sent++;

        if (net_ping(ip, 1000u, &rtt)) {
            received++;
            total_ms += rtt;
            printk("Reply from %s: bytes=32 time=%u ms seq=%u\n",
                   ip_text, (unsigned int)rtt, (unsigned int)(i + 1));
        } else {
            printk("Request timed out: seq=%u\n", (unsigned int)(i + 1));
        }
    }

    uint32_t lost = sent - received;
    printk("--- ping statistics ---\n");
    printk("%u sent, %u received, %u%% loss\n",
           (unsigned int)sent,
           (unsigned int)received,
           (unsigned int)((lost * 100u) / sent));
    if (received > 0) {
        printk("average round-trip time: %u ms\n",
               (unsigned int)(total_ms / received));
    }
}

/* ============================================================
 * nslookup
 * ============================================================ */

void netcmd_nslookup(const char *args) {
    char work[NC_LINE_MAX];
    nc_copy(work, args ? args : "", (int)sizeof(work));

    char *argv[4];
    int argc = nc_tokenize(work, argv, 4);

    if (argc < 1) {
        printk("usage: nslookup <host>\n");
        return;
    }

    if (net_init() != 0) {
        printk("nslookup: network adapter not available\n");
        return;
    }

    char dns_text[16];
    net_format_ipv4(net_info()->dns, dns_text);

    printk("Server:  %s\n", dns_text);
    printk("Address: %s\n\n", dns_text);

    uint32_t ip = 0;
    if (!net_dns_resolve(argv[0], &ip, 5000u)) {
        printk("*** %s can't find %s: DNS query failed\n", dns_text, argv[0]);
        return;
    }

    char ip_text[16];
    net_format_ipv4(ip, ip_text);
    printk("Name:    %s\n", argv[0]);
    printk("Address: %s\n", ip_text);
}
