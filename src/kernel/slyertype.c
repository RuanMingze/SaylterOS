/* ============================================================
 * slyertype.c - 内核 TrueType 平滑字体引擎
 * ------------------------------------------------------------
 * 模块分四层，职责与 FreeType 同构，实现完全独立：
 *
 *   1. 容器层：解析 sfnt 表目录，取出 head/maxp/loca/glyf/
 *      hhea/hmtx/cmap。只接受 TrueType 轮廓（glyf），
 *      不支持 CFF/OTTO 轮廓——遇到时整份字体拒收并回退点阵。
 *   2. 轮廓层：把简单字形与复合字形还原成"点 + 轮廓端点"，
 *      复合字形的平移、F2Dot14 缩放、点对齐都按规范处理。
 *   3. 曲线层：TrueType 二次贝塞尔按弦高自适应细分，
 *      细分误差控制在 1/8 像素，得到折线边表。
 *   4. 光栅层：扫描线 + 16 级垂直子采样 + 精确水平覆盖率，
 *      非零环绕规则填充；结果存成 8 位覆盖率掩码并缓存。
 *
 * 字体数据不整份读入内存：loca/cmap 这种需要随机查询的表
 * 常驻内核堆，glyf 字形数据按需 seek + read。这样 17MB 的
 * Noto Sans SC 也能在 6MB 内核堆里运行，中文由系统自己在
 * 运行时解析轮廓、自己抗锯齿，不依赖任何预生成点阵或外部
 * 渲染器。
 *
 * 所有坐标运算使用 26.6 定点（1 像素 = 64），构建参数禁用
 * SSE，禁止浮点。
 *
 * 关闭条件：vbe_is_enabled() 为假（80x25 文本模式）时
 * slyertype_init() 不做任何分配，所有绘制接口返回 0，
 * 调用方保持原有"宽字符写两个 ?"的回退行为。
 * ============================================================ */

#include <stdint.h>
#include <stddef.h>

#include "slyertype.h"
#include "vbe.h"
#include "mem.h"
#include "unicode.h"
#include "debug.h"

/* SlyzFS 桥接接口（与 auth.c / gui.c 相同的 C 入口） */
struct StDirEntry {
    uint8_t  type;
    uint8_t  flags;
    uint16_t name_len;
    char     name[256];
    uint32_t size;
    uint32_t start_lba;
    uint32_t created_ts;
    uint32_t modified_ts;
    uint16_t mode;
    uint8_t  uid;
    uint8_t  gid;
} __attribute__((packed));

extern int   slyzfs_stat(const char *path, struct StDirEntry *out);
extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_seek(void *fh, uint32_t offset);
extern void  slyzfs_close(void *fh);

/* ============================================================
 * 容量与限制
 * ============================================================ */

#define ST_MAX_FACES        2
#define ST_FONT_MAX_BYTES   (32u * 1024u * 1024u)
#define ST_LOCA_MAX_BYTES   (1024u * 1024u)
#define ST_CMAP_MAX_BYTES   (1024u * 1024u)
#define ST_GLYPH_DATA_MAX   (96u * 1024u)
#define ST_PATH_MAX         72

#define ST_GLYPH_MAX        64          /* 单个字形掩码边长上限 */
#define ST_MAX_POINTS       2048        /* 单字形轮廓点上限 */
#define ST_MAX_CONTOURS     256
#define ST_MAX_EDGES        16384       /* 展平后的折线边上限 */
#define ST_MAX_CROSSINGS    512         /* 单条采样线的交点数上限 */
#define ST_MAX_DEPTH        4           /* 复合字形递归上限 */
#define ST_SUBSAMPLES       16          /* 垂直子采样数 */
#define ST_FLATNESS         8           /* 1/8 像素的弦高阈值 */
#define ST_MAX_SUBDIV       12
#define ST_CACHE_SLOTS      160

/* 可变字体目标字重：Noto Sans SC 的默认实例是 100(Thin)，
 * 直接渲染会发灰。这里在运行时把 wght 轴解算到 Medium。 */
#define ST_TARGET_WGHT      500
#define ST_MAX_AXES         4
#define ST_MAX_TUPLES       32

/* ============================================================
 * 数据结构
 * ============================================================ */

typedef struct {
    void    *fh;                    /* 常开的 SlyzFS 文件句柄 */
    const uint8_t *mem;             /* 非空表示内嵌内存字体 */
    uint32_t mem_len;
    uint32_t file_size;

    uint8_t *dir;                   /* sfnt 表目录副本 */
    uint32_t dir_len;

    uint8_t *loca;                  /* loca 表常驻（字形偏移查询） */
    uint32_t loca_len;

    uint8_t *cmap;                  /* cmap 表常驻（码位查询） */
    uint32_t cmap_len;

    uint32_t off_glyf, len_glyf;    /* glyf 按需读取 */
    uint32_t off_hmtx, len_hmtx;    /* hmtx 按需读取 */

    uint16_t units_per_em;
    uint16_t num_glyphs;
    uint16_t num_hmetrics;
    int16_t  index_to_loc;          /* head.indexToLocFormat: 0=short,1=long */
    int16_t  ascender;
    int16_t  descender;
    int16_t  line_gap;

    uint32_t cmap4_off;             /* 相对 cmap 缓冲；0 表示不存在 */
    uint32_t cmap4_len;
    uint32_t cmap12_off;
    uint32_t cmap12_len;

    /* ---- 可变字体（fvar / avar / gvar） ----
     * Noto Sans SC 的 wght 轴默认值是 100(Thin)，直接渲染会发灰。
     * 内核在运行时把坐标解算到 Medium，再用 gvar 的增量修改轮廓，
     * 不依赖任何离线实例化产物。 */
    int      has_var;
    int      axis_count;
    int      axis_wght;             /* wght 轴下标；-1 表示没有 */
    int32_t  axis_coord;            /* 归一化坐标（F2Dot14，经 avar 映射） */
    uint32_t off_fvar, len_fvar;
    uint32_t off_avar, len_avar;
    uint32_t off_gvar, len_gvar;
    uint16_t gvar_axis_count;
    uint16_t gvar_shared_count;
    uint32_t gvar_shared_off;       /* 共享 tuple 的文件偏移 */
    uint32_t gvar_data_array;       /* 每个字形增量数据的文件偏移 */
    uint8_t *gvar_offsets;          /* glyphCount+1 个偏移 */
    uint32_t gvar_offsets_len;
    int      gvar_long_offsets;
    uint8_t *gvar_shared;           /* 共享 tuple 原始数据 */

    char path[ST_PATH_MAX];
} StFace;

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t on;                     /* 1 = 在曲线上 */
} StPoint;

typedef struct {
    StPoint  *pts;
    uint16_t *ends;                 /* 每个轮廓最后一个点在 pts 中的下标 */
    int       npts;
    int       ncontours;
} StOutline;

typedef struct {
    int32_t x0, y0, x1, y1;         /* 26.6 像素坐标 */
} StEdge;

typedef struct {
    int32_t x;                      /* 26.6 */
    int32_t dir;                    /* +1 / -1 */
} StCross;

typedef struct {
    int      used;
    int      failed;
    int      face;
    int      size;
    int      cw;                    /* 单元格宽度 */
    uint32_t cp;
    uint32_t stamp;
    int      w;
    int      h;
    int      off_x;                 /* 掩码左边相对 pen 的像素偏移 */
    int      off_y;                 /* 掩码顶部相对基线的像素偏移 */
    int      advance;               /* 压缩后的实际像素步进 */
    int      xs16;                  /* 水平缩放（16.16），1.0 = 65536 */
    uint8_t *mask;
} StCacheSlot;

/* ============================================================
 * 全局状态
 * ============================================================ */

static StFace     g_faces[ST_MAX_FACES];
static int        g_face_count;
static int        g_disk_loaded;
static int        g_work_allocated;
static int        g_ready;

static StCacheSlot g_cache[ST_CACHE_SLOTS];
static uint8_t    *g_cache_pool;
static uint32_t    g_stamp;

static StOutline  g_outlines[ST_MAX_DEPTH];
static uint8_t    *g_glyph_data[ST_MAX_DEPTH];

static StEdge    *g_edges;
static int        g_edge_count;
static int        g_edge_overflow;

static uint8_t    g_flags[ST_MAX_POINTS];
static uint16_t   g_acc[ST_GLYPH_MAX * ST_GLYPH_MAX];
static StCross    g_cross[ST_MAX_CROSSINGS];

/* 可变字体增量计算的临时区（按字形串行使用） */
static uint16_t   g_var_points_shared[ST_MAX_POINTS + 4];
static uint16_t   g_var_points_priv[ST_MAX_POINTS + 4];
static int32_t    g_var_tmp[ST_MAX_POINTS + 4];
static int32_t    g_var_dx[ST_MAX_POINTS];
static int32_t    g_var_dy[ST_MAX_POINTS];
static uint8_t    g_var_touched[ST_MAX_POINTS];

/* SlyzOS 专属字形（U+F0001）的 32x32 覆盖度。
 * 两行紧凑排布 "Sl" / "yz"，由 Ubuntu Mono 渲染后固化；
 * 这是私有区字形，不参与字体查找，也不属于任何 Unicode 文字区。 */
static const uint8_t st_logo_mask[32 * 32] = {
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,174,255,255,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,107,218,255,255,218, 92,  0,  0,  0,174,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0, 51,255,255, 38, 51,174, 64,  0,  0,  0,174,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,107,255,195,  0,  0,  0,  0,  0,  0,  0,174,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0, 25,255,255,156, 25,  0,  0,  0,  0,  0,174,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 38,139,255,255,156, 25,  0,  0,  0,174,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 12,156,255,218,  0,  0,  0,174,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 12,255,255, 51,  0,  0,174,255,107,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0, 78,195, 78,  0,107,255,255, 12,  0,  0,122,255,195,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0, 51,156,255,255,255,174, 64,  0,  0,  0, 25,218,255,255,255, 25,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,139,255,156,  0,  0,255,255, 64, 51,255,255,255,255,255,174,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0, 64,255,255, 12, 51,255,255, 12,  0,  0,  0, 38,255,255, 64,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,218,255, 78,122,255,156,  0,  0,  0, 12,218,255,122,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,107,255,174,195,255, 78,  0,  0,  0,156,255,195,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 12,255,255,255,255, 12,  0,  0, 64,255,255, 25,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,122,255,255,156,  0,  0, 12,255,255, 92,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 12,255,255, 51,  0,  0, 92,255,255,255,255,255,255,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0, 25,  0,107,255,174,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,195,255,255,174, 12,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static char       g_status[96];

/* sRGB 覆盖率校正表（gamma 2.2 的逆）。
 * 覆盖率是线性光比例，而帧缓冲做的是 sRGB 空间混合。汉字笔画在
 * 32px 下只有 1~2 像素宽，不做校正时 50% 覆盖率直接混成 128 灰，
 * 视觉上整行字发灰。把覆盖率映射到 sRGB 亮度后，笔画会恢复到
 * 接近白色的实心观感，同时保留边缘过渡。只用于 CJK 宽字形，
 * 拉丁字形保持原有灰度。 */
static const uint8_t st_gamma_lut[256] = {
    0, 21, 28, 34, 39, 43, 46, 50, 53, 56, 59, 61, 64, 66, 68, 70,
    72, 74, 76, 78, 80, 82, 84, 85, 87, 89, 90, 92, 93, 95, 96, 98,
    99, 101, 102, 103, 105, 106, 107, 109, 110, 111, 112, 114, 115, 116, 117, 118,
    119, 120, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135,
    136, 137, 138, 139, 140, 141, 142, 143, 144, 144, 145, 146, 147, 148, 149, 150,
    151, 151, 152, 153, 154, 155, 156, 156, 157, 158, 159, 160, 160, 161, 162, 163,
    164, 164, 165, 166, 167, 167, 168, 169, 170, 170, 171, 172, 173, 173, 174, 175,
    175, 176, 177, 178, 178, 179, 180, 180, 181, 182, 182, 183, 184, 184, 185, 186,
    186, 187, 188, 188, 189, 190, 190, 191, 192, 192, 193, 194, 194, 195, 195, 196,
    197, 197, 198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205, 205, 206,
    206, 207, 207, 208, 209, 209, 210, 210, 211, 212, 212, 213, 213, 214, 214, 215,
    215, 216, 217, 217, 218, 218, 219, 219, 220, 220, 221, 221, 222, 223, 223, 224,
    224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231, 232,
    232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239, 239, 240,
    240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247, 248,
    248, 249, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255
};

/* ============================================================
 * 大端读取与游标
 * ============================================================ */

static inline uint16_t st_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline int16_t st_s16(const uint8_t *p) {
    return (int16_t)st_u16(p);
}

static inline uint32_t st_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int ok;
} StCursor;

static void st_cur_init(StCursor *c, const uint8_t *p, const uint8_t *end) {
    c->p = p;
    c->end = end;
    c->ok = (p <= end);
}

static uint8_t st_cur_u8(StCursor *c) {
    if (!c->ok || c->p + 1 > c->end) { c->ok = 0; return 0; }
    return *c->p++;
}

static uint16_t st_cur_u16(StCursor *c) {
    if (!c->ok || c->p + 2 > c->end) { c->ok = 0; return 0; }
    uint16_t v = st_u16(c->p);
    c->p += 2;
    return v;
}

static int16_t st_cur_s16(StCursor *c) {
    return (int16_t)st_cur_u16(c);
}

static uint32_t st_cur_u32(StCursor *c) {
    if (!c->ok || c->p + 4 > c->end) { c->ok = 0; return 0; }
    uint32_t v = st_u32(c->p);
    c->p += 4;
    return v;
}

static int st_cur_skip(StCursor *c, uint32_t n) {
    if (!c->ok || (uint32_t)(c->end - c->p) < n) { c->ok = 0; return 0; }
    c->p += n;
    return 1;
}

/* ============================================================
 * 字体文件流式访问
 * ============================================================ */

/* 从文件绝对偏移读取 len 字节。SlyzFS 的 read 单次最多返回一个
 * 簇，因此这里循环直到读满；任何短读都视为失败。 */
static int st_raw_read(void *fh, uint32_t off, void *dst, uint32_t len) {
    if (!fh || len == 0) return len == 0;
    if (!slyzfs_seek(fh, off)) return 0;

    uint8_t *p = (uint8_t *)dst;
    uint32_t total = 0;
    while (total < len) {
        int n = slyzfs_read(fh, p + total, (int)(len - total));
        if (n <= 0) return 0;
        total += (uint32_t)n;
        if (total > len) return 0;
    }
    return 1;
}

static int st_face_read(StFace *f, uint32_t off, void *dst, uint32_t len) {
    if (f->mem) {
        if (off > f->mem_len || len > f->mem_len - off) return 0;
        const uint8_t *src = f->mem + off;
        uint8_t *d = (uint8_t *)dst;
        for (uint32_t i = 0; i < len; i++) d[i] = src[i];
        return 1;
    }
    if (off > f->file_size) return 0;
    if (len > f->file_size - off) return 0;
    return st_raw_read(f->fh, off, dst, len);
}

static int st_table_in_dir(const uint8_t *dir, uint32_t dir_len,
                           const char *tag, uint32_t *out_off,
                           uint32_t *out_len) {
    if (dir_len < 12) return 0;
    uint16_t num = st_u16(dir + 4);
    uint32_t need = 12u + (uint32_t)num * 16u;
    if (need > dir_len) return 0;

    for (uint16_t i = 0; i < num; i++) {
        const uint8_t *rec = dir + 12u + (uint32_t)i * 16u;
        if (rec[0] == (uint8_t)tag[0] && rec[1] == (uint8_t)tag[1] &&
            rec[2] == (uint8_t)tag[2] && rec[3] == (uint8_t)tag[3]) {
            *out_off = st_u32(rec + 8);
            *out_len = st_u32(rec + 12);
            return 1;
        }
    }
    return 0;
}

/* 在 cmap 的编码记录里选出最合适的子表。
 * 优先级：format 12（UCS-4） > format 4（BMP）。
 * 只接受 Unicode 平台或 Windows Unicode 编码。 */
static void st_pick_cmap(StFace *f) {
    if (f->cmap_len < 4) return;

    const uint8_t *cmap = f->cmap;
    uint16_t count = st_u16(cmap + 2);
    uint32_t need = 4u + (uint32_t)count * 8u;
    if (need > f->cmap_len) return;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *rec = cmap + 4u + (uint32_t)i * 8u;
        uint16_t platform = st_u16(rec);
        uint16_t encoding = st_u16(rec + 2);
        uint32_t sub = st_u32(rec + 4);

        int unicode = (platform == 0) ||
                      (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        if (sub + 2 > f->cmap_len) continue;

        const uint8_t *p = cmap + sub;
        uint32_t avail = f->cmap_len - sub;
        uint16_t format = st_u16(p);

        if (format == 12) {
            if (avail < 16) continue;
            uint32_t length = st_u32(p + 4);
            if (length < 16 || length > avail) continue;
            if (f->cmap12_off == 0) {
                f->cmap12_off = sub;
                f->cmap12_len = length;
            }
        } else if (format == 4) {
            if (avail < 14) continue;
            uint16_t length = st_u16(p + 2);
            if (length < 16 || (uint32_t)length > avail) continue;
            if (f->cmap4_off == 0) {
                f->cmap4_off = sub;
                f->cmap4_len = length;
            }
        }
    }
}

static void st_face_cleanup(StFace *f) {
    if (f->fh) {
        slyzfs_close(f->fh);
        f->fh = 0;
    }
    if (f->dir) { kfree(f->dir); f->dir = 0; }
    if (f->loca) { kfree(f->loca); f->loca = 0; }
    if (f->cmap) { kfree(f->cmap); f->cmap = 0; }
    if (f->gvar_offsets) { kfree(f->gvar_offsets); f->gvar_offsets = 0; }
    if (f->gvar_shared) { kfree(f->gvar_shared); f->gvar_shared = 0; }
}

/* ============================================================
 * 可变字体：fvar / avar / gvar
 * ------------------------------------------------------------
 * gvar 按字形存储 tuple 增量，不可能整表常驻。这里常驻：
 *   - fvar 轴定义（很小）；
 *   - gvar 头、共享 tuple、glyphCount+1 个偏移；
 * 每个字形的增量数据在需要时按偏移读入 g_glyph_data[depth]。
 * ============================================================ */

static int st_load_variations(StFace *f) {
    f->has_var = 0;
    f->axis_wght = -1;
    f->axis_coord = 0;
    f->axis_count = 0;

    if (!f->off_fvar || f->len_fvar < 16) return 0;

    uint8_t fh[16];
    if (!st_face_read(f, f->off_fvar, fh, 16)) return 0;

    uint16_t axis_off = st_u16(fh + 4);
    uint16_t axis_count = st_u16(fh + 8);
    uint16_t axis_size = st_u16(fh + 10);
    if (axis_count == 0 || axis_count > ST_MAX_AXES || axis_size < 20) return 0;

    uint32_t need = (uint32_t)axis_count * axis_size;
    if (axis_off > f->len_fvar || need > f->len_fvar - axis_off) return 0;

    uint8_t *axes = (uint8_t *)kmalloc(need);
    if (!axes) return 0;
    if (!st_face_read(f, f->off_fvar + axis_off, axes, need)) {
        kfree(axes);
        return 0;
    }

    f->axis_count = (int)axis_count;
    int wght = -1;
    int32_t norm = 0;
    for (int i = 0; i < (int)axis_count; i++) {
        const uint8_t *a = axes + (uint32_t)i * axis_size;
        uint32_t mn = st_u32(a + 4);
        uint32_t dflt = st_u32(a + 8);
        uint32_t mx = st_u32(a + 12);

        if (a[0] == 'w' && a[1] == 'g' && a[2] == 'h' && a[3] == 't') {
            wght = i;
            int32_t target = ST_TARGET_WGHT << 16;
            if ((uint32_t)target < mn) target = (int32_t)mn;
            if ((uint32_t)target > mx) target = (int32_t)mx;

            if ((uint32_t)target == dflt || mx == mn) {
                norm = 0;
            } else if ((uint32_t)target > dflt) {
                norm = (int32_t)(((int64_t)(target - (int32_t)dflt) << 16) /
                                 (int64_t)(mx - dflt));
            } else {
                norm = (int32_t)(-(((int64_t)((int32_t)dflt - target) << 16) /
                                   (int64_t)(dflt - mn)));
            }
        }
    }
    kfree(axes);

    if (wght < 0) return 0;
    f->axis_wght = wght;

    /* fvar 是 16.16，gvar 的 tuple 坐标是 F2Dot14。 */
    norm >>= 2;
    if (norm > 16384) norm = 16384;
    if (norm < -16384) norm = -16384;

    /* avar：把用户坐标映射到设计坐标。Noto 的 wght 轴有分段映射，
     * 不处理会让 Medium 落在错误位置。 */
    if (f->off_avar && f->len_avar >= 8) {
        uint8_t ah[8];
        if (st_face_read(f, f->off_avar, ah, 8)) {
            uint16_t ac = st_u16(ah + 6);
            if (ac == axis_count) {
                uint32_t pos = 8;
                for (int i = 0; i < (int)axis_count; i++) {
                    uint8_t scbuf[2];
                    if (pos + 2 > f->len_avar ||
                        !st_face_read(f, f->off_avar + pos, scbuf, 2)) break;
                    uint16_t segs = st_u16(scbuf);
                    uint32_t map_len = (uint32_t)(segs + 1) * 4u;
                    if (pos + 2u + map_len > f->len_avar) break;

                    if (i == wght && segs > 0) {
                        uint8_t *map = (uint8_t *)kmalloc(map_len);
                        if (map) {
                            if (st_face_read(f, f->off_avar + pos + 2u, map, map_len)) {
                                for (uint16_t s = 0; s < segs; s++) {
                                    int32_t a = st_s16(map + (uint32_t)s * 2u);
                                    int32_t b = st_s16(map + (uint32_t)(s + 1u) * 2u);
                                    if (norm >= a && norm <= b) {
                                        int32_t ma = st_s16(map + (uint32_t)(segs + 1u) * 2u + (uint32_t)s * 2u);
                                        int32_t mb = st_s16(map + (uint32_t)(segs + 1u) * 2u + (uint32_t)(s + 1u) * 2u);
                                        if (b == a) norm = mb;
                                        else norm = ma + (int32_t)(((int64_t)(mb - ma) * (norm - a)) / (b - a));
                                        break;
                                    }
                                }
                            }
                            kfree(map);
                        }
                    }
                    pos += 2u + map_len;
                }
            }
        }
    }
    f->axis_coord = norm;

    if (!f->off_gvar || f->len_gvar < 20) return 0;

    uint8_t gh[20];
    if (!st_face_read(f, f->off_gvar, gh, 20)) return 0;

    uint16_t ga = st_u16(gh + 4);
    uint16_t shared = st_u16(gh + 6);
    uint32_t shared_off = st_u32(gh + 8);
    uint16_t glyphs = st_u16(gh + 12);
    uint16_t flags = st_u16(gh + 14);
    uint32_t data_off = st_u32(gh + 16);

    if ((int)ga != f->axis_count || glyphs != f->num_glyphs) return 0;

    int lng = (flags & 1u) ? 1 : 0;
    uint32_t offsz = lng ? 4u : 2u;
    uint32_t offs_len = ((uint32_t)glyphs + 1u) * offsz;
    if (20u + offs_len > f->len_gvar) return 0;
    if (data_off > f->len_gvar) return 0;

    f->gvar_offsets = (uint8_t *)kmalloc(offs_len);
    f->gvar_shared = (uint8_t *)kmalloc((uint32_t)shared * (uint32_t)ga * 2u + 2u);
    if (!f->gvar_offsets || !f->gvar_shared) return 0;

    f->gvar_offsets_len = offs_len;
    f->gvar_long_offsets = lng;
    f->gvar_axis_count = ga;
    f->gvar_shared_count = shared;
    f->gvar_data_array = f->off_gvar + data_off;
    f->gvar_shared_off = f->off_gvar + shared_off;

    if (!st_face_read(f, f->off_gvar + 20u, f->gvar_offsets, offs_len)) return 0;

    if (shared) {
        uint32_t sl = (uint32_t)shared * (uint32_t)ga * 2u;
        if (shared_off > f->len_gvar || sl > f->len_gvar - shared_off) return 0;
        if (!st_face_read(f, f->off_gvar + shared_off, f->gvar_shared, sl)) return 0;
    }

    f->has_var = (norm != 0);
    return 1;
}

static int st_parse_face_stream(StFace *f, const char *path) {
    uint8_t hdr[12];
    if (!st_face_read(f, 0, hdr, 12)) return 0;

    uint32_t version = st_u32(hdr);
    /* 0x00010000 是 TrueType 轮廓；'true' 也合法。
     * 'OTTO' 表示 CFF 轮廓，本引擎不实现，整份拒收。 */
    if (version != 0x00010000u && version != 0x74727565u) return 0;

    uint16_t num_tables = st_u16(hdr + 4);
    if (num_tables == 0) return 0;
    uint32_t dir_len = 12u + (uint32_t)num_tables * 16u;
    if (dir_len > 1024u * 64u) return 0;

    f->dir = (uint8_t *)kmalloc(dir_len);
    if (!f->dir) return 0;
    f->dir_len = dir_len;
    if (!st_face_read(f, 0, f->dir, dir_len)) return 0;

    uint32_t off_head, len_head, off_maxp, len_maxp;
    uint32_t off_hhea, len_hhea;
    uint32_t off_loca, len_loca;
    uint32_t off_cmap, len_cmap;
    uint32_t off_fvar = 0, len_fvar = 0;
    uint32_t off_avar = 0, len_avar = 0;
    uint32_t off_gvar = 0, len_gvar = 0;

    if (!st_table_in_dir(f->dir, f->dir_len, "head", &off_head, &len_head) || len_head < 54) return 0;
    if (!st_table_in_dir(f->dir, f->dir_len, "maxp", &off_maxp, &len_maxp) || len_maxp < 6) return 0;
    if (!st_table_in_dir(f->dir, f->dir_len, "hhea", &off_hhea, &len_hhea) || len_hhea < 36) return 0;
    if (!st_table_in_dir(f->dir, f->dir_len, "hmtx", &f->off_hmtx, &f->len_hmtx)) return 0;
    if (!st_table_in_dir(f->dir, f->dir_len, "loca", &off_loca, &len_loca)) return 0;
    if (!st_table_in_dir(f->dir, f->dir_len, "glyf", &f->off_glyf, &f->len_glyf)) return 0;
    if (!st_table_in_dir(f->dir, f->dir_len, "cmap", &off_cmap, &len_cmap)) return 0;

    /* 可变字体表是可选的：静态字体没有 fvar/gvar，走默认轮廓。 */
    st_table_in_dir(f->dir, f->dir_len, "fvar", &off_fvar, &len_fvar);
    st_table_in_dir(f->dir, f->dir_len, "avar", &off_avar, &len_avar);
    st_table_in_dir(f->dir, f->dir_len, "gvar", &off_gvar, &len_gvar);
    f->off_fvar = off_fvar;
    f->len_fvar = len_fvar;
    f->off_avar = off_avar;
    f->len_avar = len_avar;
    f->off_gvar = off_gvar;
    f->len_gvar = len_gvar;

    if (f->off_glyf > f->file_size || f->len_glyf > f->file_size - f->off_glyf) return 0;
    if (f->off_hmtx > f->file_size || f->len_hmtx > f->file_size - f->off_hmtx) return 0;
    if (off_loca > f->file_size || len_loca > f->file_size - off_loca) return 0;
    if (off_cmap > f->file_size || len_cmap > f->file_size - off_cmap) return 0;
    if (len_loca < 2 || len_loca > ST_LOCA_MAX_BYTES) return 0;
    if (len_cmap < 4 || len_cmap > ST_CMAP_MAX_BYTES) return 0;
    f->loca_len = len_loca;

    uint8_t head[54];
    uint8_t maxp[6];
    uint8_t hhea[36];
    if (!st_face_read(f, off_head, head, sizeof(head))) return 0;
    if (!st_face_read(f, off_maxp, maxp, sizeof(maxp))) return 0;
    if (!st_face_read(f, off_hhea, hhea, sizeof(hhea))) return 0;

    f->units_per_em = st_u16(head + 18);
    f->index_to_loc = st_s16(head + 50);
    f->num_glyphs = st_u16(maxp + 4);
    f->num_hmetrics = st_u16(hhea + 34);
    f->ascender = st_s16(hhea + 4);
    f->descender = st_s16(hhea + 6);
    f->line_gap = st_s16(hhea + 8);

    if (f->units_per_em < 16 || f->units_per_em > 16384) return 0;
    if (f->num_glyphs == 0) return 0;
    if (f->num_hmetrics == 0 || f->num_hmetrics > f->num_glyphs) return 0;
    if (f->index_to_loc != 0 && f->index_to_loc != 1) return 0;

    uint32_t loca_need = (uint32_t)(f->num_glyphs + 1) *
                         (f->index_to_loc ? 4u : 2u);
    if (f->loca_len < loca_need) return 0;

    f->loca = (uint8_t *)kmalloc(f->loca_len);
    if (!f->loca) return 0;
    if (!st_face_read(f, off_loca, f->loca, f->loca_len)) return 0;

    f->cmap = (uint8_t *)kmalloc(len_cmap);
    if (!f->cmap) return 0;
    f->cmap_len = len_cmap;
    if (!st_face_read(f, off_cmap, f->cmap, f->cmap_len)) return 0;

    st_pick_cmap(f);
    if (f->cmap4_off == 0 && f->cmap12_off == 0) return 0;

    /* 解析可变轴并准备 gvar 增量入口；失败不影响静态字体使用。 */
    st_load_variations(f);

    int i = 0;
    while (path[i] && i < ST_PATH_MAX - 1) {
        f->path[i] = path[i];
        i++;
    }
    f->path[i] = '\0';
    return 1;
}

static int st_load_face(const char *path) {
    if (g_face_count >= ST_MAX_FACES) return 0;

    struct StDirEntry ent;
    if (!slyzfs_stat(path, &ent)) return 0;
    if (ent.size < 12 || ent.size > ST_FONT_MAX_BYTES) return 0;

    void *fh = slyzfs_open(path, "r");
    if (!fh) return 0;

    StFace *f = &g_faces[g_face_count];
    for (uint32_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;

    f->fh = fh;
    f->file_size = ent.size;

    if (!st_parse_face_stream(f, path)) {
        st_face_cleanup(f);
        return 0;
    }

    g_face_count++;
    return 1;
}

/* 内嵌在 .rodata 里的字体：调试控制台在 SlyzFS 挂载前就需要平滑拉丁字。 */
static int st_load_face_mem(const uint8_t *data, uint32_t len, const char *path) {
    if (g_face_count >= ST_MAX_FACES || !data || len < 12) return 0;

    StFace *f = &g_faces[g_face_count];
    for (uint32_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;

    f->mem = data;
    f->mem_len = len;
    f->file_size = len;

    if (!st_parse_face_stream(f, path)) {
        st_face_cleanup(f);
        return 0;
    }

    g_face_count++;
    return 1;
}

/* ============================================================
 * cmap 查询
 * ============================================================ */

static uint16_t st_gid_fmt4(const StFace *f, uint32_t cp) {
    if (cp > 0xFFFFu) return 0;

    const uint8_t *base = f->cmap + f->cmap4_off;
    uint32_t len = f->cmap4_len;
    if (len < 16) return 0;

    uint16_t seg_x2 = st_u16(base + 6);
    if (seg_x2 < 2 || (seg_x2 & 1u)) return 0;
    uint16_t seg = (uint16_t)(seg_x2 / 2);

    uint32_t end_base = 14;
    uint32_t start_base = end_base + (uint32_t)seg * 2u + 2u;
    uint32_t delta_base = start_base + (uint32_t)seg * 2u;
    uint32_t range_base = delta_base + (uint32_t)seg * 2u;
    uint32_t glyph_base = range_base + (uint32_t)seg * 2u;
    if (glyph_base > len) return 0;

    for (uint16_t i = 0; i < seg; i++) {
        uint16_t end = st_u16(base + end_base + (uint32_t)i * 2u);
        if (cp > end) continue;

        uint16_t start = st_u16(base + start_base + (uint32_t)i * 2u);
        if (cp < start) return 0;

        int16_t delta = st_s16(base + delta_base + (uint32_t)i * 2u);
        uint16_t ro = st_u16(base + range_base + (uint32_t)i * 2u);

        if (ro == 0) {
            return (uint16_t)((cp + (uint16_t)delta) & 0xFFFFu);
        }

        uint32_t addr = range_base + (uint32_t)i * 2u + ro +
                        (uint32_t)(cp - start) * 2u;
        if (addr + 2u > len) return 0;
        uint16_t gid = st_u16(base + addr);
        if (gid == 0) return 0;
        return (uint16_t)((gid + (uint16_t)delta) & 0xFFFFu);
    }
    return 0;
}

static uint16_t st_gid_fmt12(const StFace *f, uint32_t cp) {
    const uint8_t *base = f->cmap + f->cmap12_off;
    uint32_t len = f->cmap12_len;
    if (len < 16) return 0;

    uint32_t groups = st_u32(base + 12);
    if (groups > (len - 16u) / 12u) return 0;

    uint32_t lo = 0;
    uint32_t hi = groups;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        const uint8_t *g = base + 16u + mid * 12u;
        uint32_t start = st_u32(g);
        uint32_t end = st_u32(g + 4);
        if (cp < start) {
            hi = mid;
        } else if (cp > end) {
            lo = mid + 1u;
        } else {
            uint32_t gid = st_u32(g + 8) + (cp - start);
            if (gid == 0 || gid > 0xFFFFu) return 0;
            return (uint16_t)gid;
        }
    }
    return 0;
}

static uint16_t st_face_gid(const StFace *f, uint32_t cp) {
    uint16_t gid = 0;
    if (f->cmap12_off != 0) gid = st_gid_fmt12(f, cp);
    if (gid == 0 && f->cmap4_off != 0) gid = st_gid_fmt4(f, cp);
    if (gid == 0 || gid >= f->num_glyphs) return 0;
    return gid;
}

static int st_face_for(uint32_t cp) {
    for (int i = 0; i < g_face_count; i++) {
        if (st_face_gid(&g_faces[i], cp) != 0) return i;
    }
    return -1;
}

/* ============================================================
 * 字形轮廓：glyf 解析
 * ============================================================ */

static int st_glyph_range(const StFace *f, uint16_t gid,
                          uint32_t *off, uint32_t *len) {
    if (gid >= f->num_glyphs) return 0;

    uint32_t a, b;
    if (f->index_to_loc == 0) {
        uint32_t base = (uint32_t)gid * 2u;
        if (base + 4u > f->loca_len) return 0;
        a = (uint32_t)st_u16(f->loca + base) * 2u;
        b = (uint32_t)st_u16(f->loca + base + 2u) * 2u;
    } else {
        uint32_t base = (uint32_t)gid * 4u;
        if (base + 8u > f->loca_len) return 0;
        a = st_u32(f->loca + base);
        b = st_u32(f->loca + base + 4u);
    }
    if (b < a || b > f->len_glyf) return 0;

    *off = f->off_glyf + a;
    *len = b - a;
    return 1;
}

/* ============================================================
 * gvar：运行时字重变化
 * ------------------------------------------------------------
 * 只对简单字形的点集应用增量；复合字形自身的变化数据通常用于
 * 调整组件偏移，Noto Sans SC 的汉字都是简单轮廓，拉丁复合字
 * 仍能通过子字形各自应用增量获得正确粗细。
 * ============================================================ */

static uint32_t st_gvar_offset(const StFace *f, uint16_t gid) {
    if (gid > f->num_glyphs) return 0;
    if (f->gvar_long_offsets) {
        uint32_t p = (uint32_t)gid * 4u;
        if (p + 4u > f->gvar_offsets_len) return 0;
        return st_u32(f->gvar_offsets + p);
    }
    uint32_t p = (uint32_t)gid * 2u;
    if (p + 2u > f->gvar_offsets_len) return 0;
    return (uint32_t)st_u16(f->gvar_offsets + p) * 2u;
}

/* 打包点号：首字节最高位为 0 表示单个点号；
 * 为 1 表示后面跟 n 个 delta 编码的点号，n=0 表示"全部点"。 */
static int st_parse_points(const uint8_t **pp, const uint8_t *end,
                           int *out_count, uint16_t *pts) {
    if (*pp >= end) return 0;
    uint8_t control = *(*pp)++;

    if ((control & 0x80u) == 0) {
        pts[0] = control;
        *out_count = 1;
        return 1;
    }

    int n = control & 0x7Fu;
    if (n == 0) {
        *out_count = 0;         /* 全部点 */
        return 1;
    }

    uint32_t last = 0;
    for (int i = 0; i < n; i++) {
        if (*pp >= end) return 0;
        last += *(*pp)++;
        if (last > ST_MAX_POINTS + 4) return 0;
        pts[i] = (uint16_t)last;
    }
    *out_count = n;
    return 1;
}

/* 打包增量：控制字节 bit7=全零，bit6=16 位，bit0-5=数量-1。
 * F2Dot14 增量转成 16.16 后参与累加。 */
static int st_parse_deltas(const uint8_t **pp, const uint8_t *end,
                           int count, int32_t *out) {
    int got = 0;
    while (got < count) {
        if (*pp >= end) return 0;
        uint8_t control = *(*pp)++;
        int n = (control & 0x3Fu) + 1;
        if (got + n > count) n = count - got;

        if (control & 0x80u) {
            for (int i = 0; i < n; i++) out[got + i] = 0;
        } else if (control & 0x40u) {
            for (int i = 0; i < n; i++) {
                if (*pp + 2 > end) return 0;
                int16_t v = st_s16(*pp);
                *pp += 2;
                out[got + i] = (int32_t)v * 4;
            }
        } else {
            for (int i = 0; i < n; i++) {
                if (*pp >= end) return 0;
                int8_t v = (int8_t)(*(*pp)++);
                out[got + i] = (int32_t)v * 4;
            }
        }
        got += n;
    }
    return 1;
}

/* 单个 tuple 在给定归一化坐标下的标量（16.16）。
 * 只跟踪 wght 轴；其余轴保持在默认坐标 0。 */
static int32_t st_tuple_scalar(const int16_t peak[ST_MAX_AXES],
                               const int16_t start[ST_MAX_AXES],
                               const int16_t endv[ST_MAX_AXES],
                               int has_inter, int axes, int wght_axis,
                               int32_t coord) {
    int32_t scalar = 65536;

    for (int i = 0; i < axes; i++) {
        int32_t p = peak[i];
        if (p == 0) continue;

        int32_t c = (i == wght_axis) ? coord : 0;
        int32_t factor;

        if (has_inter) {
            int32_t s = start[i];
            int32_t e = endv[i];
            if (c < s || c > e) return 0;
            if (c == p) {
                factor = 65536;
            } else if (c < p) {
                if (p == s) return 0;
                factor = (int32_t)(((int64_t)(c - s) << 16) / (p - s));
            } else {
                if (e == p) return 0;
                factor = (int32_t)(((int64_t)(e - c) << 16) / (e - p));
            }
        } else {
            if (c == 0) return 0;
            if ((c > 0) != (p > 0)) return 0;
            if (c == p) {
                factor = 65536;
            } else if (c > 0) {
                if (c > p) return 0;
                factor = (int32_t)(((int64_t)c << 16) / p);
            } else {
                if (c < p) return 0;
                factor = (int32_t)(((int64_t)c << 16) / p);
            }
        }

        scalar = (int32_t)(((int64_t)scalar * factor) >> 16);
        if (scalar == 0) return 0;
    }
    return scalar;
}

static int32_t st_iup_value(int32_t c, int32_t c1, int32_t c2,
                            int32_t d1, int32_t d2) {
    if (c1 == c2) return (d1 + d2) / 2;
    int64_t t = ((int64_t)(c - c1) << 16) / (c2 - c1);
    if (t < 0) t = 0;
    if (t > 65536) t = 65536;
    return d1 + (int32_t)(((int64_t)(d2 - d1) * t) >> 16);
}

static int st_apply_gvar(StFace *f, uint16_t gid, StOutline *o, int depth) {
    if (!f->has_var || o->npts <= 0) return 1;

    uint32_t a = st_gvar_offset(f, gid);
    uint32_t b = st_gvar_offset(f, (uint16_t)(gid + 1));
    if (b <= a) return 1;
    uint32_t len = b - a;
    if (len < 4 || len > ST_GLYPH_DATA_MAX) return 1;

    /* 点集已经复制到 o；glyf 缓冲可以安全地复用为 gvar 数据缓冲。 */
    uint8_t *data = g_glyph_data[depth];
    if (!st_face_read(f, f->gvar_data_array + a, data, len)) return 1;

    const uint8_t *end = data + len;
    uint16_t tvc = st_u16(data);
    uint16_t data_off = st_u16(data + 2);
    int tuples = tvc & 0x0FFF;
    int shared_points = (tvc & 0x8000) != 0;
    if (tuples <= 0 || tuples > ST_MAX_TUPLES) return 1;
    if (data_off > len) return 1;

    int16_t peak[ST_MAX_TUPLES][ST_MAX_AXES];
    int16_t start[ST_MAX_TUPLES][ST_MAX_AXES];
    int16_t endv[ST_MAX_TUPLES][ST_MAX_AXES];
    uint16_t sizes[ST_MAX_TUPLES];
    uint8_t priv[ST_MAX_TUPLES];
    uint8_t inter[ST_MAX_TUPLES];
    int axes = f->gvar_axis_count;

    const uint8_t *hp = data + 4;
    for (int t = 0; t < tuples; t++) {
        if (hp + 4 > end) return 1;
        sizes[t] = st_u16(hp);
        uint16_t idx = st_u16(hp + 2);
        hp += 4;

        int emb = (idx & 0x8000u) != 0;
        inter[t] = (idx & 0x4000u) ? 1 : 0;
        priv[t] = (idx & 0x2000u) ? 1 : 0;
        int ti = idx & 0x0FFFu;

        if (emb) {
            if (hp + axes * 2 > end) return 1;
            for (int ax = 0; ax < axes; ax++) {
                peak[t][ax] = st_s16(hp + ax * 2);
            }
            hp += axes * 2;
        } else {
            if (ti >= f->gvar_shared_count) return 1;
            for (int ax = 0; ax < axes; ax++) {
                peak[t][ax] = st_s16(f->gvar_shared +
                                     ((uint32_t)ti * (uint32_t)axes +
                                      (uint32_t)ax) * 2u);
            }
        }

        if (inter[t]) {
            if (hp + axes * 4 > end) return 1;
            for (int ax = 0; ax < axes; ax++) {
                start[t][ax] = st_s16(hp + ax * 2);
            }
            hp += axes * 2;
            for (int ax = 0; ax < axes; ax++) {
                endv[t][ax] = st_s16(hp + ax * 2);
            }
            hp += axes * 2;
        }
    }

    for (int i = 0; i < o->npts; i++) {
        g_var_dx[i] = 0;
        g_var_dy[i] = 0;
        g_var_touched[i] = 0;
    }

    const uint8_t *s = data + data_off;
    int shared_count = 0;
    if (shared_points) {
        if (!st_parse_points(&s, end, &shared_count, g_var_points_shared)) return 1;
    }

    for (int t = 0; t < tuples; t++) {
        const uint8_t *ts = s;
        if (ts + sizes[t] > end) return 1;
        s = ts + sizes[t];

        int32_t scalar = st_tuple_scalar(peak[t], start[t], endv[t],
                                         inter[t], axes, f->axis_wght,
                                         f->axis_coord);
        if (scalar == 0) continue;

        const uint8_t *q = ts;
        const uint8_t *qend = ts + sizes[t];
        int pcount = 0;
        int all = 0;
        uint16_t *pts = g_var_points_priv;

        if (priv[t]) {
            if (!st_parse_points(&q, qend, &pcount, g_var_points_priv)) continue;
            if (pcount == 0) all = 1;
        } else if (shared_points) {
            pcount = shared_count;
            pts = g_var_points_shared;
            if (pcount == 0) all = 1;
        } else {
            all = 1;
        }

        int total = all ? (o->npts + 4) : pcount;
        if (total <= 0 || total > ST_MAX_POINTS + 4) continue;

        if (!st_parse_deltas(&q, qend, total, g_var_tmp)) continue;
        for (int k = 0; k < total; k++) {
            int pi = all ? k : (int)pts[k];
            if (pi >= o->npts) continue;
            g_var_dx[pi] += (int32_t)(((int64_t)g_var_tmp[k] * scalar) >> 16);
            g_var_touched[pi] = 1;
        }

        if (!st_parse_deltas(&q, qend, total, g_var_tmp)) continue;
        for (int k = 0; k < total; k++) {
            int pi = all ? k : (int)pts[k];
            if (pi >= o->npts) continue;
            g_var_dy[pi] += (int32_t)(((int64_t)g_var_tmp[k] * scalar) >> 16);
            g_var_touched[pi] = 1;
        }
    }

    /* IUP：未被 tuple 直接引用的点，沿轮廓在两端增量之间插值。 */
    int startp = 0;
    for (int ci = 0; ci < o->ncontours; ci++) {
        int endp = (int)o->ends[ci];
        if (endp < startp || endp >= o->npts) { startp = endp + 1; continue; }
        int n = endp - startp + 1;

        int first = -1;
        int tcount = 0;
        for (int i = 0; i < n; i++) {
            if (g_var_touched[startp + i]) {
                if (first < 0) first = i;
                tcount++;
            }
        }
        if (first < 0) { startp = endp + 1; continue; }

        if (tcount == 1) {
            int32_t dx = g_var_dx[startp + first];
            int32_t dy = g_var_dy[startp + first];
            for (int i = 0; i < n; i++) {
                if (!g_var_touched[startp + i]) {
                    g_var_dx[startp + i] = dx;
                    g_var_dy[startp + i] = dy;
                }
            }
            startp = endp + 1;
            continue;
        }

        int i = first;
        int guard = 0;
        do {
            int j = i;
            do { j = (j + 1) % n; } while (!g_var_touched[startp + j] && j != i);
            if (j == i) break;

            int steps = (j - i + n) % n;
            for (int k = 1; k < steps; k++) {
                int idx = (i + k) % n;
                if (g_var_touched[startp + idx]) continue;
                g_var_dx[startp + idx] =
                    st_iup_value(o->pts[startp + idx].x,
                                 o->pts[startp + i].x,
                                 o->pts[startp + j].x,
                                 g_var_dx[startp + i],
                                 g_var_dx[startp + j]);
                g_var_dy[startp + idx] =
                    st_iup_value(o->pts[startp + idx].y,
                                 o->pts[startp + i].y,
                                 o->pts[startp + j].y,
                                 g_var_dy[startp + i],
                                 g_var_dy[startp + j]);
            }
            i = j;
            guard++;
        } while (i != first && guard <= n);

        startp = endp + 1;
    }

    for (int i = 0; i < o->npts; i++) {
        int32_t dx = g_var_dx[i];
        int32_t dy = g_var_dy[i];
        if (dx) {
            o->pts[i].x += (dx >= 0) ? ((dx + 32768) >> 16)
                                     : -(((-dx) + 32768) >> 16);
        }
        if (dy) {
            o->pts[i].y += (dy >= 0) ? ((dy + 32768) >> 16)
                                     : -(((-dy) + 32768) >> 16);
        }
    }
    return 1;
}

static int st_outline_load(StFace *f, uint16_t gid, StOutline *out, int depth);

static int st_outline_simple(StCursor *c, StOutline *out) {
    int16_t contours = st_cur_s16(c);
    if (!c->ok) return 0;
    if (contours <= 0) {
        out->npts = 0;
        out->ncontours = 0;
        return 1;
    }
    if (contours > ST_MAX_CONTOURS) return 0;

    if (!st_cur_skip(c, 8)) return 0;   /* xMin/yMin/xMax/yMax */

    int ncont = contours;
    int last = -1;
    for (int i = 0; i < ncont; i++) {
        uint16_t end = st_cur_u16(c);
        if (!c->ok) return 0;
        if ((int)end <= last) return 0;
        out->ends[i] = end;
        last = (int)end;
    }
    int npts = last + 1;
    if (npts <= 0 || npts > ST_MAX_POINTS) return 0;

    uint16_t ins = st_cur_u16(c);
    if (!c->ok || !st_cur_skip(c, ins)) return 0;

    /* 先解 flags（含 REPEAT），再按 flags 解 x/y 增量。 */
    int i = 0;
    while (i < npts) {
        uint8_t flag = st_cur_u8(c);
        if (!c->ok) return 0;
        g_flags[i++] = flag;
        if (flag & 0x08) {
            uint8_t rep = st_cur_u8(c);
            if (!c->ok) return 0;
            while (rep-- && i < npts) {
                g_flags[i++] = flag;
            }
        }
    }

    int32_t acc = 0;
    for (i = 0; i < npts; i++) {
        uint8_t flag = g_flags[i];
        if (flag & 0x02) {
            uint8_t v = st_cur_u8(c);
            if (!c->ok) return 0;
            acc += (flag & 0x10) ? (int32_t)v : -(int32_t)v;
        } else if (!(flag & 0x10)) {
            int16_t d = st_cur_s16(c);
            if (!c->ok) return 0;
            acc += d;
        }
        out->pts[i].x = acc;
        out->pts[i].on = (uint8_t)((flag & 0x01) ? 1 : 0);
    }

    acc = 0;
    for (i = 0; i < npts; i++) {
        uint8_t flag = g_flags[i];
        if (flag & 0x04) {
            uint8_t v = st_cur_u8(c);
            if (!c->ok) return 0;
            acc += (flag & 0x20) ? (int32_t)v : -(int32_t)v;
        } else if (!(flag & 0x20)) {
            int16_t d = st_cur_s16(c);
            if (!c->ok) return 0;
            acc += d;
        }
        out->pts[i].y = acc;
    }

    out->npts = npts;
    out->ncontours = ncont;
    return 1;
}

/* 复合字形：逐组件递归加载，应用缩放/平移后追加到当前轮廓。
 * 点对齐（ARGS_ARE_XY_VALUES == 0）按规范用父/子点坐标求差。 */
static int st_outline_composite(StFace *f, const uint8_t *data, uint32_t glen,
                                StOutline *out, int depth) {
    if (depth + 1 >= ST_MAX_DEPTH) return 0;

    StCursor c;
    st_cur_init(&c, data, data + glen);
    if (!st_cur_skip(&c, 10)) return 0;   /* header 含 bbox */

    int any = 0;
    uint16_t flags;
    do {
        flags = st_cur_u16(&c);
        uint16_t cgid = st_cur_u16(&c);
        if (!c.ok || cgid >= f->num_glyphs) return 0;

        /* ARGS_ARE_XY_VALUES 时参数是有符号位移；
         * 否则参数是点号（无符号），用于父子轮廓点对齐。 */
        int have_xy = (flags & 0x0002) != 0;
        int32_t dx = 0, dy = 0;
        uint32_t pidx = 0, cidx = 0;
        if (flags & 0x0001) {
            uint16_t a = st_cur_u16(&c);
            uint16_t b = st_cur_u16(&c);
            if (!c.ok) return 0;
            if (have_xy) {
                dx = (int16_t)a;
                dy = (int16_t)b;
            } else {
                pidx = a;
                cidx = b;
            }
        } else {
            uint8_t a = st_cur_u8(&c);
            uint8_t b = st_cur_u8(&c);
            if (!c.ok) return 0;
            if (have_xy) {
                dx = (int8_t)a;
                dy = (int8_t)b;
            } else {
                pidx = a;
                cidx = b;
            }
        }

        int32_t ma = 65536, mb = 0, mc = 0, md = 65536;
        if (flags & 0x0008) {
            int16_t s = st_cur_s16(&c);
            if (!c.ok) return 0;
            ma = md = (int32_t)s * 4;
        } else if (flags & 0x0040) {
            int16_t sx = st_cur_s16(&c);
            int16_t sy = st_cur_s16(&c);
            if (!c.ok) return 0;
            ma = (int32_t)sx * 4;
            md = (int32_t)sy * 4;
        } else if (flags & 0x0080) {
            int16_t a = st_cur_s16(&c);
            int16_t b = st_cur_s16(&c);
            int16_t cc = st_cur_s16(&c);
            int16_t d = st_cur_s16(&c);
            if (!c.ok) return 0;
            ma = (int32_t)a * 4;
            mb = (int32_t)b * 4;
            mc = (int32_t)cc * 4;
            md = (int32_t)d * 4;
        }

        StOutline *child = &g_outlines[depth + 1];
        child->npts = 0;
        child->ncontours = 0;
        if (!st_outline_load(f, cgid, child, depth + 1)) return 0;

        if (!have_xy) {
            if (pidx >= (uint32_t)out->npts || cidx >= (uint32_t)child->npts) return 0;
            dx = out->pts[pidx].x - child->pts[cidx].x;
            dy = out->pts[pidx].y - child->pts[cidx].y;
        }

        int base = out->npts;
        if (base + child->npts > ST_MAX_POINTS) return 0;
        if (out->ncontours + child->ncontours > ST_MAX_CONTOURS) return 0;

        for (int i = 0; i < child->npts; i++) {
            int64_t x = child->pts[i].x;
            int64_t y = child->pts[i].y;
            int64_t tx = (x * ma + y * mc) >> 16;
            int64_t ty = (x * mb + y * md) >> 16;
            out->pts[base + i].x = (int32_t)(tx + dx);
            out->pts[base + i].y = (int32_t)(ty + dy);
            out->pts[base + i].on = child->pts[i].on;
        }
        for (int i = 0; i < child->ncontours; i++) {
            out->ends[out->ncontours + i] =
                (uint16_t)(base + child->ends[i]);
        }
        out->npts = base + child->npts;
        out->ncontours += child->ncontours;
        any = 1;

        if (flags & 0x0100) {   /* WE_HAVE_INSTRUCTIONS：跳过指令 */
            uint16_t ilen = st_cur_u16(&c);
            if (!c.ok || !st_cur_skip(&c, ilen)) return 0;
        }
    } while (flags & 0x0020);

    return any;
}

static int st_outline_load(StFace *f, uint16_t gid, StOutline *out, int depth) {
    if (depth >= ST_MAX_DEPTH) return 0;

    uint32_t goff, glen;
    if (!st_glyph_range(f, gid, &goff, &glen)) return 0;

    out->npts = 0;
    out->ncontours = 0;

    /* 空字形：合法（空格等），没有轮廓。 */
    if (glen == 0) return 1;
    if (glen > ST_GLYPH_DATA_MAX) return 0;

    uint8_t *data = g_glyph_data[depth];
    if (!st_face_read(f, goff, data, glen)) return 0;

    StCursor c;
    st_cur_init(&c, data, data + glen);

    int16_t contours = st_cur_s16(&c);
    if (!c.ok) return 0;
    if (contours >= 0) {
        st_cur_init(&c, data, data + glen);
        if (!st_outline_simple(&c, out)) return 0;
        /* 可变字体：把 wght 轴增量应用到点集，再进入展平。 */
        return st_apply_gvar(f, gid, out, depth);
    }
    return st_outline_composite(f, data, glen, out, depth);
}

/* ============================================================
 * 轮廓展平：二次贝塞尔 → 26.6 折线边
 * ============================================================ */

static inline int32_t st_scale_coord(int32_t v, int64_t scale) {
    return (int32_t)(((int64_t)v * scale) >> 16);
}

/* 字体坐标 Y 轴向上，屏幕坐标 Y 轴向下，这里统一取反。
 * 所有 y 的换算都必须经过本函数，否则字形会上下颠倒并越出单元格。 */
static inline int32_t st_scale_y(int32_t v, int64_t scale) {
    return -st_scale_coord(v, scale);
}

static inline int32_t st_abs32(int32_t v) {
    return v < 0 ? -v : v;
}

static void st_emit_line(int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1) {
    if (y0 == y1) return;
    if (g_edge_count >= ST_MAX_EDGES) {
        g_edge_overflow = 1;
        return;
    }
    StEdge *e = &g_edges[g_edge_count++];
    e->x0 = x0;
    e->y0 = y0;
    e->x1 = x1;
    e->y1 = y1;
}

static void st_emit_quad(int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1,
                         int32_t x2, int32_t y2, int depth) {
    int32_t cx = (2 * x1 - (x0 + x2)) / 2;
    int32_t cy = (2 * y1 - (y0 + y2)) / 2;

    if (depth >= ST_MAX_SUBDIV || (st_abs32(cx) + st_abs32(cy)) <= ST_FLATNESS) {
        st_emit_line(x0, y0, x2, y2);
        return;
    }

    int32_t x01 = (x0 + x1) / 2, y01 = (y0 + y1) / 2;
    int32_t x12 = (x1 + x2) / 2, y12 = (y1 + y2) / 2;
    int32_t xm = (x01 + x12) / 2, ym = (y01 + y12) / 2;

    st_emit_quad(x0, y0, x01, y01, xm, ym, depth + 1);
    st_emit_quad(xm, ym, x12, y12, x2, y2, depth + 1);
}

static void st_flatten_contour(const StOutline *o, int start, int end,
                               int64_t scale_x, int64_t scale_y) {
    int n = end - start + 1;
    if (n <= 0) return;

    int first_on = -1;
    for (int i = 0; i < n; i++) {
        if (o->pts[start + i].on) { first_on = i; break; }
    }

    if (first_on < 0) {
        /* 全离线点：以最后一点与第一点的中点为起点，
         * 每两个控制点之间插入隐含的曲线点。 */
        int32_t px = (st_scale_coord(o->pts[start].x, scale_x) +
                      st_scale_coord(o->pts[end].x, scale_x)) / 2;
        int32_t py = (st_scale_y(o->pts[start].y, scale_y) +
                      st_scale_y(o->pts[end].y, scale_y)) / 2;
        int32_t sx = px, sy = py;

        for (int k = 0; k < n; k++) {
            const StPoint *p = &o->pts[start + k];
            const StPoint *q = &o->pts[start + ((k + 1) % n)];
            int32_t cx = st_scale_coord(p->x, scale_x);
            int32_t cy = st_scale_y(p->y, scale_y);
            int32_t mx = (cx + st_scale_coord(q->x, scale_x)) / 2;
            int32_t my = (cy + st_scale_y(q->y, scale_y)) / 2;

            st_emit_quad(px, py, cx, cy, mx, my, 0);
            px = mx;
            py = my;
        }
        st_emit_line(px, py, sx, sy);
        return;
    }

    int32_t start_x = st_scale_coord(o->pts[start + first_on].x, scale_x);
    int32_t start_y = st_scale_y(o->pts[start + first_on].y, scale_y);
    int32_t px = start_x;
    int32_t py = start_y;

    int idx = (first_on + 1) % n;
    int steps = 0;
    while (steps < n) {
        const StPoint *p = &o->pts[start + idx];
        int32_t cx = st_scale_coord(p->x, scale_x);
        int32_t cy = st_scale_y(p->y, scale_y);

        if (p->on) {
            st_emit_line(px, py, cx, cy);
            px = cx;
            py = cy;
            idx = (idx + 1) % n;
            steps++;
            continue;
        }

        int nidx = (idx + 1) % n;
        const StPoint *q = &o->pts[start + nidx];
        int32_t qx = st_scale_coord(q->x, scale_x);
        int32_t qy = st_scale_y(q->y, scale_y);

        if (q->on) {
            st_emit_quad(px, py, cx, cy, qx, qy, 0);
            px = qx;
            py = qy;
            idx = (nidx + 1) % n;
            steps += 2;
        } else {
            int32_t mx = (cx + qx) / 2;
            int32_t my = (cy + qy) / 2;
            st_emit_quad(px, py, cx, cy, mx, my, 0);
            px = mx;
            py = my;
            idx = nidx;
            steps++;
        }
    }

    st_emit_line(px, py, start_x, start_y);
}

/* ============================================================
 * 扫描线光栅化
 * ============================================================ */

static inline int32_t st_floor_px(int32_t v26) {
    if (v26 >= 0) return v26 >> 6;
    return -(((-v26) + 63) >> 6);
}

static inline int32_t st_ceil_px(int32_t v26) {
    if (v26 >= 0) return (v26 + 63) >> 6;
    return -((-v26) >> 6);
}

static int st_cross_cmp(const StCross *a, const StCross *b) {
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    return 0;
}

static void st_add_span(uint16_t *row, int w, int32_t sx, int32_t ex) {
    if (sx < 0) sx = 0;
    int32_t limit = w * 64;
    if (ex > limit) ex = limit;
    if (ex <= sx) return;

    int p0 = (int)(sx >> 6);
    int p1 = (int)(ex >> 6);
    if (p0 >= w) return;
    if (p1 > w) p1 = w;

    if (p0 == p1) {
        row[p0] = (uint16_t)(row[p0] + (ex - sx));
        return;
    }

    row[p0] = (uint16_t)(row[p0] + (64 - (sx & 63)));
    for (int i = p0 + 1; i < p1; i++) {
        row[i] = (uint16_t)(row[i] + 64);
    }
    int32_t tail = ex & 63;
    if (tail != 0 && p1 < w) {
        row[p1] = (uint16_t)(row[p1] + tail);
    }
}

/* 把一个字形光栅化进 slot。返回 1 成功（含空字形），0 失败。
 * xs16 是水平压缩系数：纵向按 size 缩放，横向再乘 xs16，
 * 使等宽字体在 16px 栅格里放得下又不缩高度。 */
static int st_rasterize(StFace *f, uint16_t gid, int size, int xs16,
                        int gamma, StCacheSlot *slot) {
    int64_t scale_y = ((int64_t)size << 22) / (int64_t)f->units_per_em;
    int64_t scale_x = (scale_y * (int64_t)xs16) >> 16;

    StOutline *o = &g_outlines[0];
    if (!st_outline_load(f, gid, o, 0)) return 0;

    g_edge_count = 0;
    g_edge_overflow = 0;

    int start = 0;
    for (int ci = 0; ci < o->ncontours; ci++) {
        int end = (int)o->ends[ci];
        if (end < start || end >= o->npts) return 0;
        st_flatten_contour(o, start, end, scale_x, scale_y);
        start = end + 1;
    }

    if (g_edge_overflow) return 0;

    if (g_edge_count == 0) {
        slot->w = 0;
        slot->h = 0;
        slot->off_x = 0;
        slot->off_y = 0;
        return 1;
    }

    int32_t minx = g_edges[0].x0, maxx = g_edges[0].x0;
    int32_t miny = g_edges[0].y0, maxy = g_edges[0].y0;
    for (int i = 0; i < g_edge_count; i++) {
        const StEdge *e = &g_edges[i];
        int32_t xs[2] = { e->x0, e->x1 };
        int32_t ys[2] = { e->y0, e->y1 };
        for (int k = 0; k < 2; k++) {
            if (xs[k] < minx) minx = xs[k];
            if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k];
            if (ys[k] > maxy) maxy = ys[k];
        }
    }

    int32_t bx0 = st_floor_px(minx);
    int32_t bx1 = st_ceil_px(maxx);
    int32_t by0 = st_floor_px(miny);
    int32_t by1 = st_ceil_px(maxy);

    int w = (int)(bx1 - bx0);
    int h = (int)(by1 - by0);
    if (w <= 0 || h <= 0) {
        slot->w = 0;
        slot->h = 0;
        slot->off_x = 0;
        slot->off_y = 0;
        return 1;
    }
    if (w > ST_GLYPH_MAX || h > ST_GLYPH_MAX) return 0;

    uint32_t cells = (uint32_t)w * (uint32_t)h;
    for (uint32_t i = 0; i < cells; i++) g_acc[i] = 0;

    for (int row = 0; row < h; row++) {
        for (int s = 0; s < ST_SUBSAMPLES; s++) {
            int32_t ys = (by0 + row) * 64 + s * (64 / ST_SUBSAMPLES) +
                         (64 / (ST_SUBSAMPLES * 2));

            int nc = 0;
            int overflow = 0;
            for (int i = 0; i < g_edge_count; i++) {
                const StEdge *e = &g_edges[i];
                int inside = (e->y0 <= ys && e->y1 > ys) ||
                             (e->y1 <= ys && e->y0 > ys);
                if (!inside) continue;
                if (nc >= ST_MAX_CROSSINGS) { overflow = 1; break; }

                int32_t dy = e->y1 - e->y0;
                int64_t t = ((int64_t)(ys - e->y0) << 16) / (int64_t)dy;
                int64_t x = (int64_t)e->x0 +
                            ((int64_t)(e->x1 - e->x0) * t >> 16);
                g_cross[nc].x = (int32_t)x;
                g_cross[nc].dir = (dy > 0) ? 1 : -1;
                nc++;
            }
            if (overflow) return 0;

            /* 插入排序：单条扫描线交点数很小。 */
            for (int i = 1; i < nc; i++) {
                StCross key = g_cross[i];
                int j = i - 1;
                while (j >= 0 && st_cross_cmp(&g_cross[j], &key) > 0) {
                    g_cross[j + 1] = g_cross[j];
                    j--;
                }
                g_cross[j + 1] = key;
            }

            int wind = 0;
            uint16_t *accrow = &g_acc[(uint32_t)row * (uint32_t)w];
            for (int k = 0; k + 1 < nc; k++) {
                wind += g_cross[k].dir;
                if (wind != 0) {
                    st_add_span(accrow, w,
                                g_cross[k].x - bx0 * 64,
                                g_cross[k + 1].x - bx0 * 64);
                }
            }
        }
    }

    uint8_t *mask = slot->mask;
    const uint32_t denom = (uint32_t)(ST_SUBSAMPLES * 64);
    for (uint32_t i = 0; i < cells; i++) {
        uint32_t a = ((uint32_t)g_acc[i] * 255u + denom / 2u) / denom;
        if (a > 255u) a = 255u;
        if (gamma) a = st_gamma_lut[a];
        mask[i] = (uint8_t)a;
    }

    slot->w = w;
    slot->h = h;
    slot->off_x = (int)bx0;
    slot->off_y = (int)by0;
    return 1;
}

/* ============================================================
 * 缓存
 * ============================================================ */

/* 从 hmtx 读取原始 advance（字体单位）。hmtx 不常驻内存：
 * 只有字形首次进入缓存时才读 2 字节。 */
static int st_raw_advance(StFace *f, uint16_t gid, uint32_t *adv) {
    uint32_t rec;
    if (gid < f->num_hmetrics) {
        rec = f->off_hmtx + (uint32_t)gid * 4u;
    } else {
        rec = f->off_hmtx + (uint32_t)(f->num_hmetrics - 1u) * 4u;
    }
    uint8_t b[2];
    if (!st_face_read(f, rec, b, 2)) return 0;
    *adv = st_u16(b);
    return 1;
}

static StCacheSlot *st_cache_get(uint32_t cp, int size, int cw, int face,
                                 uint16_t gid, int gamma) {
    StCacheSlot *slot = 0;

    for (int i = 0; i < ST_CACHE_SLOTS; i++) {
        StCacheSlot *s = &g_cache[i];
        if (s->used && s->cp == cp && s->size == size &&
            s->cw == cw && s->face == face) {
            s->stamp = ++g_stamp;
            return s->failed ? 0 : s;
        }
    }

    for (int i = 0; i < ST_CACHE_SLOTS; i++) {
        if (!g_cache[i].used) { slot = &g_cache[i]; break; }
    }
    if (!slot) {
        slot = &g_cache[0];
        for (int i = 1; i < ST_CACHE_SLOTS; i++) {
            if (g_cache[i].stamp < slot->stamp) slot = &g_cache[i];
        }
    }

    slot->used = 1;
    slot->failed = 0;
    slot->face = face;
    slot->size = size;
    slot->cw = cw;
    slot->cp = cp;
    slot->stamp = ++g_stamp;
    slot->w = 0;
    slot->h = 0;
    slot->off_x = 0;
    slot->off_y = 0;

    if (gid == 0) {
        slot->failed = 1;
        return 0;
    }

    StFace *f = &g_faces[face];
    uint32_t raw = 0;
    if (!st_raw_advance(f, gid, &raw)) {
        slot->failed = 1;
        return 0;
    }

    int raw_px = (int)((raw * (uint32_t)size +
                        (uint32_t)f->units_per_em / 2u) /
                       (uint32_t)f->units_per_em);
    int xs16 = 65536;
    if (raw_px > cw && raw_px > 0) {
        xs16 = (int)(((int64_t)cw << 16) / raw_px);
        if (xs16 < 16384) xs16 = 16384;
    }
    int advance = (int)(((int64_t)raw_px * xs16) >> 16);
    if (advance > cw) advance = cw;

    slot->xs16 = xs16;
    slot->advance = advance;

    if (!st_rasterize(f, gid, size, xs16, gamma, slot)) {
        slot->failed = 1;
        return 0;
    }
    return slot;
}

/* ============================================================
 * 单元格绘制
 * ============================================================ */

static void st_vertical(const StFace *f, int size, int *asc, int *desc) {
    int upem = (int)f->units_per_em;
    int a = (int)(((int64_t)f->ascender * size + upem / 2) / upem);
    int d = (int)(((int64_t)(-f->descender) * size + upem / 2) / upem);
    if (a < 1) a = 1;
    if (d < 0) d = 0;
    *asc = a;
    *desc = d;
}

/* SlyzOS 私有区字形：32x32 固定掩码，在单元格内居中绘制。 */
static int st_draw_logo(int x, int y, int cw, int ch,
                        uint32_t fg, uint32_t bg, int to_fb, int opaque) {
    if (!vbe_is_enabled() || cw <= 0 || ch <= 0) return 0;

    if (opaque) {
        if (to_fb) {
            vbe_fb_fill_rect((uint16_t)x, (uint16_t)y,
                             (uint16_t)cw, (uint16_t)ch, bg);
        } else {
            vbe_fill_rect((uint16_t)x, (uint16_t)y,
                          (uint16_t)cw, (uint16_t)ch, bg);
        }
    }

    int dx = x + (cw - 32) / 2;
    int dy = y + (ch - 32) / 2;

    int mx0 = 0, my0 = 0, mx1 = 32, my1 = 32;
    if (dx < x) mx0 = x - dx;
    if (dy < y) my0 = y - dy;
    if (dx + 32 > x + cw) mx1 = x + cw - dx;
    if (dy + 32 > y + ch) my1 = y + ch - dy;
    if (mx1 <= mx0 || my1 <= my0) return 1;

    const uint8_t *mask = st_logo_mask + my0 * 32 + mx0;
    int w = mx1 - mx0;
    int h = my1 - my0;

    if (to_fb) {
        vbe_draw_mask_fb(dx + mx0, dy + my0, w, h, mask, 32, fg, bg);
    } else {
        vbe_draw_mask_buf(dx + mx0, dy + my0, w, h, mask, 32, fg);
    }
    return 1;
}

static int st_draw(int to_fb, int opaque,
                   int x, int y, int cw, int ch,
                   uint32_t cp, uint32_t fg, uint32_t bg) {
    if (!g_ready || cw <= 0 || ch <= 0) return 0;
    if (!vbe_is_enabled()) return 0;

    /* SlyzOS 专属字形优先于任何字体查找。 */
    if (cp == SLYERTYPE_LOGO_CP) {
        return st_draw_logo(x, y, cw, ch, fg, bg, to_fb, opaque);
    }

    int face = st_face_for(cp);
    if (face < 0) return 0;

    StFace *f = &g_faces[face];
    uint16_t gid = st_face_gid(f, cp);
    if (gid == 0) return 0;

    int wide = (utf32_width(cp) == 2);
    int size;
    int baseline;

    if (wide) {
        /* CJK 字形按 em 方框设计，直接把 em 铺满单元格高度，
         * 基线放在 7/8 处（CJK 的下伸区约占 1/8 em）。 */
        size = ch;
        if (size < 6) size = 6;
        if (size > 160) size = 160;
        baseline = y + (size * 7) / 8;
    } else {
        /* 拉丁字形按 hhea 行高反推字号，让字面高度与点阵一致。 */
        int span = (int)f->ascender - (int)f->descender;
        if (span <= 0) span = (int)f->units_per_em;
        size = (ch * (int)f->units_per_em + span / 2) / span;
        if (size < 6) size = 6;
        if (size > 160) size = 160;

        int asc, desc;
        st_vertical(f, size, &asc, &desc);
        baseline = y + (ch + asc - desc) / 2;
    }

    StCacheSlot *s = st_cache_get(cp, size, cw, face, gid, wide);
    if (!s) return 0;

    int pen_x = x + (cw - s->advance) / 2;
    if (pen_x < x) pen_x = x;

    if (opaque) {
        if (to_fb) {
            vbe_fb_fill_rect((uint16_t)x, (uint16_t)y,
                             (uint16_t)cw, (uint16_t)ch, bg);
        } else {
            vbe_fill_rect((uint16_t)x, (uint16_t)y,
                          (uint16_t)cw, (uint16_t)ch, bg);
        }
    }

    if (s->w <= 0 || s->h <= 0) return 1;

    int dx = pen_x + s->off_x;
    int dy = baseline + s->off_y;

    /* 裁到单元格，避免字形伸进相邻字符或标题栏。 */
    int mx0 = 0, my0 = 0, mx1 = s->w, my1 = s->h;
    if (dx < x) mx0 = x - dx;
    if (dy < y) my0 = y - dy;
    if (dx + s->w > x + cw) mx1 = x + cw - dx;
    if (dy + s->h > y + ch) my1 = y + ch - dy;
    if (mx1 <= mx0 || my1 <= my0) return 1;

    const uint8_t *mask = s->mask + (uint32_t)my0 * (uint32_t)s->w + (uint32_t)mx0;
    int w = mx1 - mx0;
    int h = my1 - my0;

    if (to_fb) {
        vbe_draw_mask_fb(dx + mx0, dy + my0, w, h, mask, s->w, fg, bg);
    } else {
        vbe_draw_mask_buf(dx + mx0, dy + my0, w, h, mask, s->w, fg);
    }
    return 1;
}

int slyertype_draw_cell_buf(int x, int y, int cw, int ch,
                            uint32_t cp, uint32_t fg) {
    return st_draw(0, 0, x, y, cw, ch, cp, fg, 0);
}

int slyertype_draw_cell_buf_opaque(int x, int y, int cw, int ch,
                                   uint32_t cp, uint32_t fg, uint32_t bg) {
    return st_draw(0, 1, x, y, cw, ch, cp, fg, bg);
}

int slyertype_draw_cell_fb(int x, int y, int cw, int ch,
                           uint32_t cp, uint32_t fg, uint32_t bg) {
    return st_draw(1, 1, x, y, cw, ch, cp, fg, bg);
}

/* ============================================================
 * 初始化与状态
 * ============================================================ */

static void st_set_status(const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(g_status) - 1) {
        g_status[i] = s[i];
        i++;
    }
    g_status[i] = '\0';
}

static int st_alloc_work(void) {
    if (g_work_allocated) return 1;

    g_cache_pool = (uint8_t *)kmalloc(ST_CACHE_SLOTS * ST_GLYPH_MAX * ST_GLYPH_MAX);
    g_edges = (StEdge *)kmalloc(ST_MAX_EDGES * sizeof(StEdge));
    if (!g_cache_pool || !g_edges) return 0;

    for (int i = 0; i < ST_MAX_DEPTH; i++) {
        StOutline *o = &g_outlines[i];
        o->pts = (StPoint *)kmalloc(ST_MAX_POINTS * sizeof(StPoint));
        o->ends = (uint16_t *)kmalloc(ST_MAX_CONTOURS * sizeof(uint16_t));
        g_glyph_data[i] = (uint8_t *)kmalloc(ST_GLYPH_DATA_MAX);
        if (!o->pts || !o->ends || !g_glyph_data[i]) return 0;
        o->npts = 0;
        o->ncontours = 0;
    }

    for (int i = 0; i < ST_CACHE_SLOTS; i++) {
        g_cache[i].used = 0;
        g_cache[i].failed = 0;
        g_cache[i].mask = g_cache_pool +
                          (uint32_t)i * ST_GLYPH_MAX * ST_GLYPH_MAX;
    }
    g_stamp = 0;
    g_work_allocated = 1;
    return 1;
}

int slyertype_init_early(void) {
#if SAYLTER_DEBUG
    extern const unsigned char slyertype_embedded_ttf[];
    extern const unsigned char slyertype_embedded_ttf_end[];

    if (g_ready) return 1;

    if (!vbe_is_enabled()) {
        st_set_status("off (text mode)");
        return 0;
    }
    if (!st_alloc_work()) {
        st_set_status("off (out of memory)");
        return 0;
    }

    uint32_t len = (uint32_t)(slyertype_embedded_ttf_end -
                              slyertype_embedded_ttf);
    if (g_face_count == 0) {
        if (st_load_face_mem(slyertype_embedded_ttf, len,
                             "embedded:ubuntu-mono")) {
            g_ready = 1;
            st_set_status("active (embedded Ubuntu Mono)");
        }
    }
    return g_ready;
#else
    return 0;
#endif
}

int slyertype_init(void) {
    if (g_disk_loaded) return g_ready;
    g_disk_loaded = 1;

    /* 非显卡模式：整套引擎关闭，不做任何分配。 */
    if (!vbe_is_enabled()) {
        st_set_status("off (text mode)");
        return 0;
    }

    if (!st_alloc_work()) {
        st_set_status("off (out of memory)");
        return 0;
    }

#if SAYLTER_DEBUG
    /* 调试模式下拉丁字体已经内嵌，磁盘阶段只补 CJK 面。 */
    if (g_face_count == 0) (void)slyertype_init_early();
    static const char *candidates[] = {
        "system\\fonts\\noto-sans-sc.ttf",
        "system\\fonts\\saylter-sans.ttf"
    };
#else
    static const char *candidates[] = {
        "system\\fonts\\ubuntu-mono.ttf",
        "system\\fonts\\noto-sans-sc.ttf",
        "system\\fonts\\saylter-sans.ttf"
    };
#endif

    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (st_load_face(candidates[i])) {
            if (g_face_count >= ST_MAX_FACES) break;
        }
    }

    if (g_face_count == 0) {
        st_set_status("off (no TrueType font)");
        return 0;
    }

    g_ready = 1;
    st_set_status(g_face_count > 1 ? "active (2 faces)"
                                   : "active (1 face)");
    return 1;
}

int slyertype_available(void) {
    return g_ready;
}

int slyertype_face_count(void) {
    return g_face_count;
}

int slyertype_covers(uint32_t cp) {
    if (!g_ready) return 0;
    return st_face_for(cp) >= 0;
}

const char *slyertype_status(void) {
    if (g_status[0] == '\0') return "uninitialized";
    return g_status;
}
