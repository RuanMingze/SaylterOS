/* ============================================================
 * net.h - 有线网络：E1000 驱动 + TCP/IP 协议栈 + 命令行客户端
 * ------------------------------------------------------------
 * 设计约束：
 *   1. 内核是单地址空间、协作式调度，网络栈用同步轮询实现，
 *      不注册中断，避免与 PIC 级联/EOI 的既有约定纠缠；
 *   2. 无 libc、无浮点：所有校验和与时间换算都是整数运算；
 *   3. 收发缓冲区全部走静态 BSS 或 kmalloc，不碰 VBE backbuffer
 *      （0xA00000）与内核堆（0x400000..0xA00000）；
 *   4. 输出到屏幕的字符串必须是纯 ASCII。
 *
 * 分层：
 *   E1000(legacy RX/TX 描述符环) -> Ethernet -> ARP -> IPv4
 *   -> ICMP/UDP(DNS)/TCP(客户端) -> HTTP/1.1 -> shell 命令
 * ============================================================ */

#ifndef NET_H
#define NET_H

#include <stdint.h>

/* ---------- 网络配置与统计 ----------
 * IPv4 地址统一使用主机字节序（0x0A00020F = 10.0.2.15），
 * 只有在写入报文时才通过 htonl32 转成网络字节序。 */
struct NetInfo {
    int      initialized;      /* net_init 已经跑过一次（不代表成功） */
    int      present;          /* 找到并初始化了 E1000 */
    int      link_up;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint64_t mmio_base;

    uint8_t  mac[6];

    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;

    uint32_t speed_mbps;

    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_errors;
    uint32_t tx_errors;

    uint32_t arp_requests;
    uint32_t arp_replies;
    uint32_t arp_misses;
    uint32_t ip_rx;
    uint32_t ip_tx;
    uint32_t icmp_rx;
    uint32_t icmp_tx;
    uint32_t udp_rx;
    uint32_t udp_tx;
    uint32_t tcp_rx;
    uint32_t tcp_tx;
};

/* 探测并初始化网卡。成功返回 0；没有找到网卡返回 -1；
 * 找到但初始化失败返回 -2。可重复调用，第一次的结果会被缓存。 */
int net_init(void);

/* 网卡是否可用（已找到且链路已就绪） */
int net_ready(void);

struct NetInfo *net_info(void);

/* 修改静态 IPv4 配置（主机字节序）。参数为 0 表示保持原值。 */
void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns);

/* 毫秒级单调时钟（TSC 校准，PIT 提供基准） */
uint32_t net_now_ms(void);

/* 点分十进制转换。成功返回 1。 */
int net_parse_ipv4(const char *text, uint32_t *out);
void net_format_ipv4(uint32_t ip, char out[16]);
void net_format_mac(const uint8_t mac[6], char out[18]);

/* 轮询网卡：处理所有已到达的报文（ARP/ICMP/UDP/TCP 内部回调）。
 * 阻塞式 API 在等待期间会反复调用它。 */
void net_poll(void);

/* ARP 解析：把 IP 解析成 MAC，超时返回 0 */
int net_arp_resolve(uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms);

/* ICMP echo。成功返回 1 并给出往返毫秒数。 */
int net_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms);

/* DNS A 记录查询。host 可以是点分十进制字面量（直接返回）。 */
int net_dns_resolve(const char *host, uint32_t *ip_out, uint32_t timeout_ms);

/* TCP 客户端。
 *   connect 成功返回 0；
 *   send 返回已发送字节数，失败 -1；
 *   recv 返回 >0 字节数、0 表示对端关闭、-1 表示超时/错误。 */
int  net_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);
int  net_tcp_send(const void *data, uint32_t len, uint32_t timeout_ms);
int  net_tcp_recv(void *buf, uint32_t buflen, uint32_t timeout_ms);
void net_tcp_close(void);

/* HTTP GET（GUI 浏览器）。
 * out/cap 是响应体缓冲（建议 >= 64KB），头部会在缓冲内就地前移覆盖。
 * status_out 返回 HTTP 状态码（拿不到时为 0），ctype_out 返回 Content-Type。
 * 返回响应体长度；-1 = URL/连接失败；-2 = 响应头不完整。
 * 只支持 http://，不跟随重定向，不发送 Accept-Encoding（避免 gzip）。 */
int net_http_get(const char *url, char *out, int cap, int *status_out,
                 char *ctype_out, int ctype_cap);

/* ---------- shell 命令入口 ---------- */
void netcmd_ip(const char *args);
void netcmd_curl(const char *args);
void netcmd_ping(const char *args);
void netcmd_nslookup(const char *args);

#endif /* NET_H */
