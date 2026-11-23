/* ============================================================
 * i2c_tp.c - Intel LPSS I2C 主控制器 + HID over I2C 触控板
 * ------------------------------------------------------------
 * 适用机型：触控板不接 8042 辅助端口，而是以 ACPI\PNP0C50
 * （HID over I2C）挂在 Intel LPSS I2C 控制器上，例如
 * Lenovo 昭阳 K42-80 的 \_SB.PCI0.I2C0.TPD1。
 *
 * 实现依据（公开规范，不参考任何实现代码）：
 *   - ACPI 6.x：_CRS 的 I2cSerialBus 资源、_DSM、_ADR、_STA
 *   - Microsoft“HID over I2C Protocol Specification”：
 *     HID 描述符 30 字节、输入寄存器（2 字节长度 + 报告）、
 *     命令寄存器（2 字节操作码 + 2 字节参数）
 *   - Synopsys DesignWare APB I2C 数据手册：IC_* 寄存器
 *   - USB HID 1.11：报告描述符（Item）解析
 *
 * 设计取舍：
 *   - 完全轮询：不配置 GPIO 中断，避免额外依赖 IOAPIC/GPIO 驱动；
 *   - 坐标归一到 0..6143 后交给 mouse.c 的统一手势引擎；
 *   - 任何一步失败都只降低功能，不崩溃：记录日志并定时重试，
 *     因此 Fn+F6 关闭再打开触控板后可以自动恢复。
 * ============================================================ */

#include <acpi.h>
#include <stdint.h>

#include "io.h"
#include "i2c_tp.h"
#include "mouse.h"
#include "net.h"
#include "tty.h"

/* ============================================================
 * PCI 配置空间（bus 0..255, dev 0..31, fn 0..7）
 * ============================================================ */

#define PCI_CFG_ADDR  0xCF8u
#define PCI_CFG_DATA  0xCFCu

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t a = 0x80000000u |
                 ((uint32_t)bus << 16) |
                 ((uint32_t)dev << 11) |
                 ((uint32_t)fn << 8) |
                 (uint32_t)(off & 0xFC);
    outl(PCI_CFG_ADDR, a);
    return inl(PCI_CFG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint32_t v) {
    uint32_t a = 0x80000000u |
                 ((uint32_t)bus << 16) |
                 ((uint32_t)dev << 11) |
                 ((uint32_t)fn << 8) |
                 (uint32_t)(off & 0xFC);
    outl(PCI_CFG_ADDR, a);
    outl(PCI_CFG_DATA, v);
}

/* ============================================================
 * DesignWare APB I2C 寄存器（公开数据手册）
 * ============================================================ */

#define DW_IC_CON            0x00u
#define DW_IC_TAR            0x04u
#define DW_IC_DATA_CMD       0x10u
#define DW_IC_SS_SCL_HCNT    0x14u
#define DW_IC_SS_SCL_LCNT    0x18u
#define DW_IC_FS_SCL_HCNT    0x1Cu
#define DW_IC_FS_SCL_LCNT    0x20u
#define DW_IC_INTR_MASK      0x30u
#define DW_IC_RAW_INTR_STAT  0x34u
#define DW_IC_RX_TL          0x38u
#define DW_IC_TX_TL          0x3Cu
#define DW_IC_CLR_INTR       0x40u
#define DW_IC_CLR_TX_ABRT    0x54u
#define DW_IC_ENABLE         0x6Cu
#define DW_IC_STATUS         0x70u
#define DW_IC_TXFLR          0x74u
#define DW_IC_RXFLR          0x78u
#define DW_IC_SDA_HOLD       0x7Cu
#define DW_IC_TX_ABRT_SOURCE 0x80u
#define DW_IC_ENABLE_STATUS  0x9Cu
#define DW_IC_COMP_TYPE      0xFCu

#define DW_STATUS_ACTIVITY   0x00000001u
#define DW_STATUS_TFNF       0x00000002u
#define DW_STATUS_TFE        0x00000004u
#define DW_STATUS_RFNE       0x00000008u

#define DW_DATA_READ         0x00000100u
#define DW_DATA_STOP         0x00000200u
#define DW_DATA_RESTART      0x00000400u

#define DW_COMP_TYPE_VALUE   0x44570140u

/* ============================================================
 * HID 报告描述符解析
 * ============================================================ */

#define HID_MAX_FIELDS       160
#define HID_MAX_REPORT_IDS   256

typedef struct {
    uint8_t  report_id;
    uint8_t  bit_size;
    uint32_t bit_off;
    int32_t  lmin;
    int32_t  lmax;
    uint16_t page;
    uint16_t usage;
} HID_FIELD;

typedef struct {
    /* 设备连接信息 */
    int      ready;
    int      tried;
    int      fail_logged;
    int      init_fail;
    uint32_t last_try_ms;
    uint32_t last_read_ms;
    int      io_errors;

    /* I2C */
    uint8_t  bus, dev, fn;
    uint16_t slave;
    uint32_t speed;
    uint16_t slave_addr_raw;
    uint32_t bar;
    volatile uint32_t *mmio;

    /* HID 描述符 */
    uint16_t hid_desc_reg;
    uint16_t report_desc_reg;
    uint16_t report_desc_len;
    uint16_t input_reg;
    uint16_t max_input;
    uint16_t command_reg;
    uint16_t data_reg;
    uint16_t vendor_id;
    uint16_t product_id;

    /* 报告字段 */
    int      has_report_id;
    HID_FIELD fields[HID_MAX_FIELDS];
    int      nfields;
    uint16_t max_contacts;

    /* 上一帧坐标（无接触上报时保持位置） */
    int      last_x, last_y;
} TP_DEV;

static TP_DEV g_tp;
static uint8_t g_rep_desc[512];
static uint8_t g_in_buf[192];

/* ============================================================
 * MMIO 访问
 * ============================================================ */

static uint32_t dw_read(uint32_t off) {
    return g_tp.mmio[off >> 2];
}

static void dw_write(uint32_t off, uint32_t v) {
    g_tp.mmio[off >> 2] = v;
}

static void tp_delay_ms(uint32_t ms) {
    uint32_t t0 = net_now_ms();
    while ((uint32_t)(net_now_ms() - t0) < ms) {
        __asm__ volatile ("pause");
    }
}

static int dw_wait(uint32_t off, uint32_t mask, int want_set, uint32_t timeout_ms) {
    uint32_t t0 = net_now_ms();
    for (;;) {
        uint32_t st = dw_read(off);
        if (want_set ? ((st & mask) == mask) : ((st & mask) == 0)) return 1;
        if ((uint32_t)(net_now_ms() - t0) > timeout_ms) return 0;
    }
}

static void dw_clear_abort(void) {
    (void)dw_read(DW_IC_CLR_TX_ABRT);
    (void)dw_read(DW_IC_CLR_INTR);
}

/* ============================================================
 * I2C 传输（寄存器地址写 + 可选重复起始读）
 * ============================================================ */

static int i2c_write(uint16_t reg, const uint8_t *data, int len) {
    int total = 2 + len;
    int i;

    if (!dw_wait(DW_IC_STATUS, DW_STATUS_ACTIVITY, 0, 50)) return -1;
    dw_clear_abort();

    for (i = 0; i < total; i++) {
        uint32_t v;
        uint32_t flags = 0;

        if (i == 0) v = reg & 0xFF;
        else if (i == 1) v = (reg >> 8) & 0xFF;
        else v = data[i - 2];
        if (i == total - 1) flags |= DW_DATA_STOP;

        if (!dw_wait(DW_IC_STATUS, DW_STATUS_TFNF, 1, 50)) return -1;
        dw_write(DW_IC_DATA_CMD, v | flags);
    }

    if (!dw_wait(DW_IC_STATUS, DW_STATUS_TFE, 1, 50)) return -1;
    if (!dw_wait(DW_IC_STATUS, DW_STATUS_ACTIVITY, 0, 50)) return -1;
    if (dw_read(DW_IC_TX_ABRT_SOURCE) != 0) {
        dw_clear_abort();
        return -1;
    }
    return 0;
}

static int i2c_read(uint16_t reg, uint8_t *out, int len) {
    int issued = 0;
    int got = 0;

    if (!dw_wait(DW_IC_STATUS, DW_STATUS_ACTIVITY, 0, 50)) return -1;
    dw_clear_abort();

    /* 写寄存器地址；最后一个地址字节后发重复起始 */
    if (!dw_wait(DW_IC_STATUS, DW_STATUS_TFNF, 1, 50)) return -1;
    dw_write(DW_IC_DATA_CMD, (uint32_t)(reg & 0xFF));
    if (!dw_wait(DW_IC_STATUS, DW_STATUS_TFNF, 1, 50)) return -1;
    dw_write(DW_IC_DATA_CMD,
             (uint32_t)((reg >> 8) & 0xFF) | DW_DATA_RESTART);

    while (got < len) {
        uint32_t st = dw_read(DW_IC_STATUS);

        /* 小批量下发读命令，避免 RX FIFO 溢出 */
        while (issued < len &&
               (uint32_t)(issued - got) < 8 &&
               (st & DW_STATUS_TFNF)) {
            uint32_t flags = DW_DATA_READ;
            if (issued == len - 1) flags |= DW_DATA_STOP;
            dw_write(DW_IC_DATA_CMD, flags);
            issued++;
            st = dw_read(DW_IC_STATUS);
        }

        if (st & DW_STATUS_RFNE) {
            out[got++] = (uint8_t)(dw_read(DW_IC_DATA_CMD) & 0xFF);
            continue;
        }

        if (dw_read(DW_IC_TX_ABRT_SOURCE) != 0) {
            dw_clear_abort();
            return -1;
        }
        if (issued == len && !(st & DW_STATUS_ACTIVITY) &&
            !(st & DW_STATUS_RFNE)) {
            break;
        }
        __asm__ volatile ("pause");
    }

    if (got != len) return -1;
    if (!dw_wait(DW_IC_STATUS, DW_STATUS_ACTIVITY, 0, 50)) return -1;
    if (dw_read(DW_IC_TX_ABRT_SOURCE) != 0) {
        dw_clear_abort();
        return -1;
    }
    return 0;
}

/* ============================================================
 * 控制器初始化
 * ============================================================ */

static int dw_controller_init(uint32_t speed) {
    uint32_t con;

    dw_write(DW_IC_ENABLE, 0);
    (void)dw_wait(DW_IC_ENABLE_STATUS, 0x1, 0, 20);

    con = 0x00000001u |          /* 主机模式 */
          0x00000040u |          /* 关闭从机 */
          0x00000020u |          /* 允许重复起始 */
          0x00000100u;           /* TX FIFO 空时置位状态位 */
    if (speed >= 400000u) {
        con |= 0x00000004u;      /* 快速模式 */
        dw_write(DW_IC_FS_SCL_HCNT, 120);
        dw_write(DW_IC_FS_SCL_LCNT, 140);
    } else {
        con |= 0x00000002u;      /* 标准模式 */
        dw_write(DW_IC_SS_SCL_HCNT, 470);
        dw_write(DW_IC_SS_SCL_LCNT, 530);
    }
    dw_write(DW_IC_CON, con);
    dw_write(DW_IC_TAR, g_tp.slave_addr_raw & 0x7FF);
    dw_write(DW_IC_RX_TL, 0);
    dw_write(DW_IC_TX_TL, 0);
    dw_write(DW_IC_SDA_HOLD, 0x0001000F);
    dw_clear_abort();

    dw_write(DW_IC_ENABLE, 0x1);
    return dw_wait(DW_IC_ENABLE_STATUS, 0x1, 1, 50);
}

/* LPSS 私有寄存器区：公开资料说明控制器上电后处于复位/门控状态，
 * 需要软件释放。只在 COMP_TYPE 不可读时才尝试，且把读到的原值写回日志，
 * 便于真机排查。 */
static int lpss_prepare(void) {
    volatile uint32_t *priv = (volatile uint32_t *)((uintptr_t)g_tp.bar + 0x800);
    uint32_t ct;

    ct = dw_read(DW_IC_COMP_TYPE);
    if (ct == DW_COMP_TYPE_VALUE) return 1;

    printk("[TP] LPSS not ready, ct=");
    printk("%x", ct);
    printk(", priv=");
    printk("%x", priv[0]);
    printk("/");
    printk("%x", priv[1]);
    printk("\n");

    /* 释放复位并打开时钟（低两位属于本控制器功能） */
    priv[1] = priv[1] & ~0x3u;
    priv[0] = priv[0] & ~0x3u;
    tp_delay_ms(10);

    ct = dw_read(DW_IC_COMP_TYPE);
    printk("[TP] after LPSS release, ct=");
    printk("%x", ct);
    printk("\n");
    return ct == DW_COMP_TYPE_VALUE;
}

/* ============================================================
 * ACPI 辅助
 * ============================================================ */

static int acpi_eval_int(ACPI_HANDLE h, const char *name, uint64_t *out) {
    ACPI_BUFFER buf;
    ACPI_OBJECT *obj;
    ACPI_STATUS st;

    buf.Length = ACPI_ALLOCATE_BUFFER;
    buf.Pointer = NULL;
    st = AcpiEvaluateObject(h, (ACPI_STRING)name, NULL, &buf);
    if (ACPI_FAILURE(st) || !buf.Pointer) return 0;

    obj = (ACPI_OBJECT *)buf.Pointer;
    if (obj->Type != ACPI_TYPE_INTEGER) {
        AcpiOsFree(buf.Pointer);
        return 0;
    }
    *out = (uint64_t)obj->Integer.Value;
    AcpiOsFree(buf.Pointer);
    return 1;
}

static ACPI_HANDLE g_found_dev;

static ACPI_STATUS tp_find_cb(ACPI_HANDLE obj, UINT32 depth, void *ctx,
                              void **rv) {
    (void)depth;
    (void)ctx;
    (void)rv;
    if (!g_found_dev) g_found_dev = obj;
    return AE_OK;
}

/* HID 描述符寄存器地址：_DSM(UUID 3cdff6f7-..., Rev 1, Func 1) */
static uint16_t acpi_hid_desc_addr(ACPI_HANDLE dev) {
    uint8_t uuid[16] = { 0xF7, 0xF6, 0xDF, 0x3C,
                         0x67, 0x42, 0x55, 0x45,
                         0xAD, 0x05,
                         0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
    ACPI_OBJECT args[3];
    ACPI_OBJECT_LIST list;
    ACPI_BUFFER out;
    ACPI_STATUS st;
    uint16_t addr = 0x0001;

    args[0].Type = ACPI_TYPE_BUFFER;
    args[0].Buffer.Length = 16;
    args[0].Buffer.Pointer = uuid;
    args[1].Type = ACPI_TYPE_INTEGER;
    args[1].Integer.Value = 1;
    args[2].Type = ACPI_TYPE_INTEGER;
    args[2].Integer.Value = 1;

    list.Count = 3;
    list.Pointer = args;
    out.Length = ACPI_ALLOCATE_BUFFER;
    out.Pointer = NULL;

    st = AcpiEvaluateObject(dev, (ACPI_STRING)"_DSM", &list, &out);
    if (ACPI_SUCCESS(st) && out.Pointer) {
        ACPI_OBJECT *obj = (ACPI_OBJECT *)out.Pointer;
        if (obj->Type == ACPI_TYPE_INTEGER) {
            addr = (uint16_t)(obj->Integer.Value & 0xFFFF);
        }
        AcpiOsFree(out.Pointer);
    }
    return addr ? addr : 0x0001;
}

/* 在 bus 上按厂商/设备号找 Intel LPSS I2C（9D60..9D63 等） */
static int pci_find_i2c(uint8_t bus, uint8_t *out_dev, uint8_t *out_fn) {
    uint8_t d, f;
    for (d = 0; d < 32; d++) {
        for (f = 0; f < 8; f++) {
            uint32_t id = pci_read32(bus, d, f, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);
            if (vendor != 0x8086) continue;
            if (device >= 0x9D60 && device <= 0x9D63) {
                *out_dev = d;
                *out_fn = f;
                return 1;
            }
        }
    }
    return 0;
}

/* ============================================================
 * 报告描述符解析
 * ============================================================ */

static uint32_t g_bits[HID_MAX_REPORT_IDS];

static void hid_add_field(uint8_t rid, uint32_t bit, uint8_t size,
                          int32_t lmin, int32_t lmax, uint16_t page,
                          uint16_t usage) {
    HID_FIELD *f;

    if (g_tp.nfields >= HID_MAX_FIELDS) return;
    if (size == 0 || size > 32) return;

    f = &g_tp.fields[g_tp.nfields++];
    f->report_id = rid;
    f->bit_off = bit;
    f->bit_size = size;
    f->lmin = lmin;
    f->lmax = lmax;
    f->page = page;
    f->usage = usage;
}

static void hid_parse_report_desc(const uint8_t *d, int len) {
    int i = 0;
    uint16_t page = 0;
    uint32_t rsize = 0, rcount = 0;
    int32_t lmin = 0, lmax = 0;
    uint8_t rid = 0;
    uint16_t usages[32];
    int nusages = 0;
    uint16_t umin = 0, umax = 0;

    g_tp.nfields = 0;
    g_tp.has_report_id = 0;
    g_tp.max_contacts = 0;
    {
        int k;
        for (k = 0; k < HID_MAX_REPORT_IDS; k++) g_bits[k] = 0;
    }

    while (i < len) {
        uint8_t prefix = d[i++];
        int size, type, tag;
        uint32_t val = 0;
        int b;

        if (prefix == 0xFE) {          /* 长项目 */
            int dsize;
            if (i >= len) break;
            dsize = d[i];
            i += 1 + dsize;
            continue;
        }

        size = prefix & 0x03;
        if (size == 3) size = 4;
        type = (prefix >> 2) & 0x03;
        tag = (prefix >> 4) & 0x0F;

        for (b = 0; b < size; b++) {
            if (i >= len) break;
            val |= (uint32_t)d[i++] << (8 * b);
        }

        if (type == 1) {               /* 全局项目 */
            switch (tag) {
            case 0x0: page = (uint16_t)val; break;
            case 0x1:                       /* Logical Minimum */
                lmin = (size == 1) ? (int8_t)val :
                       (size == 2) ? (int16_t)val : (int32_t)val;
                break;
            case 0x2:                       /* Logical Maximum */
                lmax = (size == 1) ? (int8_t)val :
                       (size == 2) ? (int16_t)val : (int32_t)val;
                break;
            case 0x7: rsize = val; break;
            case 0x8:
                rid = (uint8_t)val;
                g_tp.has_report_id = 1;
                break;
            case 0x9: rcount = val; break;
            default: break;
            }
        } else if (type == 2) {        /* 局部项目 */
            if (tag == 0x0) {
                if (nusages < 32) usages[nusages++] = (uint16_t)val;
            } else if (tag == 0x1) {
                umin = (uint16_t)val;
            } else if (tag == 0x2) {
                umax = (uint16_t)val;
            }
        } else if (type == 0) {        /* 主项目 */
            if (tag == 0x8) {          /* Input */
                uint32_t k;
                uint32_t bit = g_bits[rid & (HID_MAX_REPORT_IDS - 1)];

                for (k = 0; k < rcount; k++) {
                    uint16_t usage;
                    if (nusages > 0) {
                        int idx = (k < (uint32_t)nusages) ? (int)k : nusages - 1;
                        usage = usages[idx];
                    } else if (umax >= umin) {
                        usage = (uint16_t)(umin + k);
                    } else {
                        usage = 0;
                    }
                    hid_add_field(rid, bit, (uint8_t)rsize, lmin, lmax,
                                  page, usage);
                    bit += rsize;
                }
                g_bits[rid & (HID_MAX_REPORT_IDS - 1)] = bit;
            } else if (tag == 0xA || tag == 0xB || tag == 0x9) {
                /* Collection / Feature / Output：只推进位偏移 */
                if (tag == 0xB) {
                    g_bits[rid & (HID_MAX_REPORT_IDS - 1)] += rcount * rsize;
                }
            }
            nusages = 0;
            umin = umax = 0;
        }
    }

    /* 统计接触点上限 */
    {
        int k;
        for (k = 0; k < g_tp.nfields; k++) {
            HID_FIELD *f = &g_tp.fields[k];
            if (f->page == 0x0D && f->usage == 0x51) {
                g_tp.max_contacts++;
            }
        }
    }
}

static uint32_t hid_field_value(const HID_FIELD *f, const uint8_t *rep,
                                int rep_len) {
    uint32_t v = 0;
    int b;

    if (!f) return 0;
    for (b = 0; b < f->bit_size && b < 32; b++) {
        uint32_t bit = f->bit_off + (uint32_t)b;
        if ((int)(bit / 8) >= rep_len) break;
        if (rep[bit / 8] & (1u << (bit & 7))) v |= (1u << b);
    }
    return v;
}

/* 取第 idx 个满足 page/usage 且报告 ID 匹配的字段 */
static const HID_FIELD *hid_nth(uint8_t rid, uint16_t page, uint16_t usage,
                                int idx) {
    int k;
    int n = 0;

    for (k = 0; k < g_tp.nfields; k++) {
        HID_FIELD *f = &g_tp.fields[k];
        if (f->report_id != rid) continue;
        if (f->page != page || f->usage != usage) continue;
        if (n == idx) return f;
        n++;
    }
    return NULL;
}

static int hid_field_count(uint8_t rid, uint16_t page, uint16_t usage) {
    int k, n = 0;
    for (k = 0; k < g_tp.nfields; k++) {
        HID_FIELD *f = &g_tp.fields[k];
        if (f->report_id == rid && f->page == page && f->usage == usage) n++;
    }
    return n;
}

static int scale_axis(const HID_FIELD *f, uint32_t raw) {
    int64_t v = (int64_t)raw;
    int64_t lo = f->lmin;
    int64_t hi = f->lmax;

    if (hi <= lo) {
        if (v < 0) v = 0;
        if (v > 6143) v = 6143;
        return (int)v;
    }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)(((v - lo) * 6143) / (hi - lo));
}

/* ============================================================
 * HID over I2C 协议
 * ============================================================ */

static int hid_command(uint16_t opcode, uint8_t arg) {
    uint8_t c[4];
    c[0] = (uint8_t)(opcode & 0xFF);
    c[1] = (uint8_t)(opcode >> 8);
    c[2] = arg;
    c[3] = 0;
    return i2c_write(g_tp.command_reg, c, 4);
}

static int hid_read_descriptor(void) {
    uint8_t hdr[4];
    uint8_t desc[40];
    int dlen;
    const uint8_t *p;

    if (i2c_read(g_tp.hid_desc_reg, hdr, 4) != 0) return -1;
    dlen = hdr[0] | (hdr[1] << 8);
    if (dlen < 10 || dlen > (int)sizeof(desc)) {
        printk("[TP] bad HID descriptor length\n");
        return -1;
    }
    if (i2c_read(g_tp.hid_desc_reg, desc, dlen) != 0) return -1;

    p = desc;
    g_tp.report_desc_len    = (uint16_t)(p[4] | (p[5] << 8));
    g_tp.report_desc_reg    = (uint16_t)(p[6] | (p[7] << 8));
    g_tp.input_reg          = (uint16_t)(p[8] | (p[9] << 8));
    g_tp.max_input          = (uint16_t)(p[10] | (p[11] << 8));
    g_tp.command_reg        = (uint16_t)(p[16] | (p[17] << 8));
    g_tp.data_reg           = (uint16_t)(p[18] | (p[19] << 8));
    g_tp.vendor_id          = (uint16_t)(p[20] | (p[21] << 8));
    g_tp.product_id         = (uint16_t)(p[22] | (p[23] << 8));

    if (g_tp.report_desc_len == 0 || g_tp.report_desc_len > sizeof(g_rep_desc) ||
        g_tp.input_reg == 0 || g_tp.max_input == 0 || g_tp.command_reg == 0) {
        printk("[TP] HID descriptor invalid\n");
        return -1;
    }

    printk("[TP] HID desc: rdesc=");
    printk("%x", g_tp.report_desc_len);
    printk(" in=");
    printk("%x", g_tp.input_reg);
    printk(" cmd=");
    printk("%x", g_tp.command_reg);
    printk(" vid=");
    printk("%x", g_tp.vendor_id);
    printk(" pid=");
    printk("%x", g_tp.product_id);
    printk("\n");

    return 0;
}

/* 把一帧 HID 输入报告转成统一触控板数据 */
static void tp_handle_report(const uint8_t *rep, int len) {
    uint8_t rid = 0;
    int off = 0;
    int count = 0;
    int k;
    int best = -1;
    int x = g_tp.last_x;
    int y = g_tp.last_y;
    int z = 0;
    int wenc = 2;
    int phys = 0;
    int slots;

    if (len <= 0) return;
    if (g_tp.has_report_id) {
        rid = rep[0];
        off = 1;
    }
    if (len <= off) return;
    rep += off;
    len -= off;

    /* 物理按键 */
    for (k = 0; k < 3; k++) {
        const HID_FIELD *f = hid_nth(rid, 0x09, (uint16_t)(k + 1), 0);
        if (f && (hid_field_value(f, rep, len) & 1)) phys |= (1 << k);
    }

    /* 接触点数量 */
    {
        const HID_FIELD *cc = hid_nth(rid, 0x0D, 0x54, 0);
        if (cc) count = (int)hid_field_value(cc, rep, len);
    }

    slots = hid_field_count(rid, 0x0D, 0x42);   /* Tip Switch 槽位数 */
    if (slots <= 0) slots = g_tp.max_contacts;
    if (slots > 16) slots = 16;

    if (count <= 0 && slots > 0) {
        /* 没有 Contact Count 字段：按 Tip Switch 统计 */
        for (k = 0; k < slots; k++) {
            const HID_FIELD *f = hid_nth(rid, 0x0D, 0x42, k);
            if (f && (hid_field_value(f, rep, len) & 1)) {
                count++;
                if (best < 0) best = k;
            }
        }
    } else if (count > 0 && best < 0) {
        for (k = 0; k < slots; k++) {
            const HID_FIELD *f = hid_nth(rid, 0x0D, 0x42, k);
            if (!f) continue;
            if (hid_field_value(f, rep, len) & 1) { best = k; break; }
        }
        if (best < 0) best = 0;
    }

    if (count > 0) {
        const HID_FIELD *fx = hid_nth(rid, 0x01, 0x30, best);
        const HID_FIELD *fy = hid_nth(rid, 0x01, 0x31, best);
        const HID_FIELD *fp = hid_nth(rid, 0x0D, 0x30, best);
        const HID_FIELD *fw = hid_nth(rid, 0x0D, 0x48, best);
        uint32_t vw = fw ? hid_field_value(fw, rep, len) : 0;

        if (fx) x = scale_axis(fx, hid_field_value(fx, rep, len));
        if (fy) y = scale_axis(fy, hid_field_value(fy, rep, len));
        g_tp.last_x = x;
        g_tp.last_y = y;

        if (fp) z = (int)hid_field_value(fp, rep, len);
        if (z < 25) z = 60;            /* 无压力字段/量程过小时给固定值 */

        /* 宽度用于手掌误触；否则按手指数编码 */
        if (fw && vw >= 10) {
            wenc = 12;
        } else if (count == 1) {
            wenc = 2;
        } else if (count == 2) {
            wenc = 0;
        } else {
            wenc = 1;
        }
    } else {
        z = 0;
    }

    tp_input_packet(x, y, z, wenc, phys, net_now_ms());
}

static int tp_read_input(void) {
    int max = g_tp.max_input;
    int n;
    int rlen;

    if (max > (int)sizeof(g_in_buf)) max = (int)sizeof(g_in_buf);
    if (max < 2) max = 2;

    n = i2c_read(g_tp.input_reg, g_in_buf, max);
    if (n != 0) return -1;

    rlen = g_in_buf[0] | (g_in_buf[1] << 8);
    if (rlen <= 0) return 0;
    if (rlen > max - 2) rlen = max - 2;

    tp_handle_report(g_in_buf + 2, rlen);
    return 1;
}

/* ============================================================
 * 设备初始化
 * ============================================================ */

static int i2c_tp_init(void) {
    ACPI_HANDLE dev = NULL;
    ACPI_HANDLE ctrl = NULL;
    ACPI_BUFFER res;
    ACPI_RESOURCE *r;
    ACPI_STATUS st;
    char *ctrl_path = NULL;
    uint64_t adr = 0;
    uint64_t bbn = 0;
    uint64_t sta = 0;
    uint8_t bus = 0;
    uint8_t pdev = 0, pfn = 0;
    uint32_t id;
    uint32_t bar;
    uint16_t cmd;
    int found = 0;

    g_found_dev = NULL;
    st = AcpiGetDevices((ACPI_STRING)"PNP0C50", tp_find_cb, NULL, NULL);
    if (ACPI_SUCCESS(st) && g_found_dev) dev = g_found_dev;
    if (!dev) {
        /* 兼容部分固件不提供 _CID 的情况 */
        (void)AcpiGetHandle(NULL, (ACPI_STRING)"\\_SB.PCI0.I2C0.TPD1", &dev);
    }
    if (!dev) return 0;

    if (acpi_eval_int(dev, "_STA", &sta)) {
        if ((sta & 0x0B) != 0x0B) {
            if (g_tp.fail_logged < 3) {
                g_tp.fail_logged++;
                printk("[TP] ACPI _STA disabled\n");
            }
            return 0;
        }
    }

    g_tp.hid_desc_reg = acpi_hid_desc_addr(dev);

    res.Length = ACPI_ALLOCATE_BUFFER;
    res.Pointer = NULL;
    st = AcpiGetCurrentResources(dev, &res);
    if (ACPI_FAILURE(st) || !res.Pointer) return 0;

    for (r = (ACPI_RESOURCE *)res.Pointer;
         r->Type != ACPI_RESOURCE_TYPE_END_TAG;
         r = ACPI_NEXT_RESOURCE(r)) {
        if (r->Type != ACPI_RESOURCE_TYPE_SERIAL_BUS) continue;
        if (r->Data.CommonSerialBus.Type != ACPI_RESOURCE_SERIAL_TYPE_I2C) continue;
        g_tp.slave_addr_raw = r->Data.I2cSerialBus.SlaveAddress;
        g_tp.speed = r->Data.I2cSerialBus.ConnectionSpeed;
        if (r->Data.I2cSerialBus.ResourceSource.StringPtr) {
            ctrl_path = r->Data.I2cSerialBus.ResourceSource.StringPtr;
        }
        found = 1;
        break;
    }
    AcpiOsFree(res.Pointer);

    if (!found) return 0;

    if (ctrl_path) {
        (void)AcpiGetHandle(NULL, (ACPI_STRING)ctrl_path, &ctrl);
    }
    if (!ctrl) {
        (void)AcpiGetHandle(NULL, (ACPI_STRING)"\\_SB.PCI0.I2C0", &ctrl);
    }

    if (ctrl) {
        if (!acpi_eval_int(ctrl, "_STA", &sta) || (sta & 0x09) == 0x09) {
            if (acpi_eval_int(ctrl, "_ADR", &adr)) {
                pdev = (uint8_t)((adr >> 16) & 0x1F);
                pfn = (uint8_t)(adr & 0x7);
            }
        }
    }

    if (AcpiGetHandle(NULL, (ACPI_STRING)"\\_SB.PCI0", &ctrl) == AE_OK) {
        if (acpi_eval_int(ctrl, "_BBN", &bbn)) bus = (uint8_t)bbn;
    }

    /* 用 _ADR 找不到时按 Intel LPSS I2C 设备号扫描 */
    if (pci_read32(bus, pdev, pfn, 0x00) == 0xFFFFFFFFu) {
        if (!pci_find_i2c(bus, &pdev, &pfn)) {
            if (g_tp.fail_logged < 3) {
                g_tp.fail_logged++;
                printk("[TP] no Intel LPSS I2C controller\n");
            }
            return 0;
        }
    }

    id = pci_read32(bus, pdev, pfn, 0x00);
    if ((id & 0xFFFF) != 0x8086) {
        if (!pci_find_i2c(bus, &pdev, &pfn)) return 0;
        id = pci_read32(bus, pdev, pfn, 0x00);
    }

    cmd = (uint16_t)(pci_read32(bus, pdev, pfn, 0x04) & 0xFFFF);
    cmd |= 0x0002;                     /* 打开内存空间译码 */
    pci_write32(bus, pdev, pfn, 0x04, (uint32_t)(cmd | 0x0004));

    bar = pci_read32(bus, pdev, pfn, 0x10) & 0xFFFFFFF0u;
    if ((pci_read32(bus, pdev, pfn, 0x10) & 0x6) == 0x4) {
        /* 64 位 BAR：高 32 位通常为 0（LPSS 位于 4GB 以下） */
        (void)pci_read32(bus, pdev, pfn, 0x14);
    }
    if (bar == 0) {
        printk("[TP] I2C BAR0 missing\n");
        return 0;
    }

    g_tp.bus = bus;
    g_tp.dev = pdev;
    g_tp.fn = pfn;
    g_tp.bar = bar;
    g_tp.mmio = (volatile uint32_t *)(uintptr_t)bar;

    printk("[TP] I2C controller pci=");
    printk("%x", (uint32_t)id);
    printk(" bar=");
    printk("%x", bar);
    printk(" slave=");
    printk("%x", g_tp.slave_addr_raw);
    printk(" speed=");
    printk("%u", (unsigned)g_tp.speed);
    printk("\n");

    if (!lpss_prepare()) {
        printk("[TP] LPSS controller unavailable\n");
        return 0;
    }
    if (!dw_controller_init(g_tp.speed)) {
        printk("[TP] I2C controller init failed\n");
        return 0;
    }

    if (hid_read_descriptor() != 0) return 0;

    /* 上电 → 主机复位 → 等待复位应答（输入寄存器返回 0 长度） */
    (void)hid_command(0x0008, 1);
    tp_delay_ms(20);
    (void)hid_command(0x0001, 0);
    {
        uint8_t t[4];
        int tries;
        for (tries = 0; tries < 10; tries++) {
            tp_delay_ms(10);
            if (i2c_read(g_tp.input_reg, t, 4) == 0) {
                if ((t[0] | (t[1] << 8)) == 0) break;
            }
        }
    }

    if (i2c_read(g_tp.report_desc_reg, g_rep_desc,
                 g_tp.report_desc_len) != 0) {
        printk("[TP] report descriptor read failed\n");
        return 0;
    }

    hid_parse_report_desc(g_rep_desc, g_tp.report_desc_len);

    printk("[TP] report desc parsed: fields=");
    printk("%u", (unsigned)g_tp.nfields);
    printk(" contacts=");
    printk("%u", (unsigned)g_tp.max_contacts);
    printk(" rid=");
    printk("%u", (unsigned)g_tp.has_report_id);
    printk("\n");

    /* 至少要有 X/Y 才能真正工作 */
    if (!hid_nth(0, 0x01, 0x30, 0)) {
        int k, ok = 0;
        for (k = 0; k < HID_MAX_REPORT_IDS; k++) {
            if (hid_nth((uint8_t)k, 0x01, 0x30, 0)) { ok = 1; break; }
        }
        if (!ok) {
            printk("[TP] no X axis in report descriptor\n");
            return 0;
        }
    }

    g_tp.last_x = 3071;
    g_tp.last_y = 3071;
    g_tp.io_errors = 0;
    printk("[TP] touchpad ready\n");
    return 1;
}

/* ============================================================
 * 对外轮询入口
 * ============================================================ */

void i2c_tp_poll(void) {
    uint32_t now = net_now_ms();

    if (!g_tp.ready) {
        if (g_tp.tried && (uint32_t)(now - g_tp.last_try_ms) < 2000u) return;
        g_tp.tried = 1;
        g_tp.last_try_ms = now;
        if (!i2c_tp_init()) {
            g_tp.init_fail++;
            return;
        }
        g_tp.ready = 1;
    }

    if ((uint32_t)(now - g_tp.last_read_ms) < 3u) return;
    g_tp.last_read_ms = now;

    if (tp_read_input() < 0) {
        g_tp.io_errors++;
        if (g_tp.io_errors >= 30) {
            /* 设备被 Fn+F6 关闭或总线异常：退回未就绪，稍后重新初始化 */
            g_tp.ready = 0;
            g_tp.tried = 0;
            g_tp.io_errors = 0;
            printk("[TP] device lost, will retry\n");
        }
    } else {
        g_tp.io_errors = 0;
    }
}
