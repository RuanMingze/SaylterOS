/* ============================================================
 * slyzlink.h - Slyz 模块链接格式（.slyzt / .slyzd）
 * ------------------------------------------------------------
 * 固定基址加载模型：
 *   - 模块在构建期声明 load_addr，运行期不会搬移；
 *   - 动态链接只做“加载期符号解析 + 绝对地址回填”，不需要 PIC；
 *   - 静态链接在构建期合并依赖，产物没有导入表。
 *
 * 现有 .slys 是固定地址裸机模块；.slyzt 在其之上增加符号表，
 * .slyzd 再增加导入表、重定位表与依赖表。三者共用同一套
 * 64 字节起始头布局，用 magic 区分，加载器可以统一解析。
 *
 * 本文件只定义磁盘格式，不包含加载器实现。
 * ============================================================ */

#ifndef SLYZLINK_H
#define SLYZLINK_H

#include <stdint.h>

#define SLYZT_MAGIC0 'S'
#define SLYZT_MAGIC1 'L'
#define SLYZT_MAGIC2 'Y'
#define SLYZT_MAGIC3 'Z'
#define SLYZT_MAGIC4 'T'
#define SLYZT_MAGIC5 0x00
#define SLYZT_MAGIC6 0x00
#define SLYZT_MAGIC7 0x00

#define SLYZD_MAGIC0 'S'
#define SLYZD_MAGIC1 'L'
#define SLYZD_MAGIC2 'Y'
#define SLYZD_MAGIC3 'Z'
#define SLYZD_MAGIC4 'D'
#define SLYZD_MAGIC5 0x00
#define SLYZD_MAGIC6 0x00
#define SLYZD_MAGIC7 0x00

#define SLYZLINK_VERSION       1u
/* 头包含 magic、18 个字段与 32 字节模块名，packed 后正好 104 字节。 */
#define SLYZLINK_HEADER_SIZE   104u

/* flags */
#define SLYZ_MOD_DYNAMIC       0x0001u   /* 带导入/重定位 */
#define SLYZ_MOD_SHARED        0x0002u   /* 允许被多个模块依赖 */
#define SLYZ_MOD_KERNEL_API    0x0004u   /* 入口接收 SlysKernelApi */

/* 符号类型 */
#define SLYZ_SYM_FUNC          0u
#define SLYZ_SYM_DATA          1u
#define SLYZ_SYM_RODATA        2u

/* 重定位类型 */
#define SLYZ_RELOC_ABS64       1u
#define SLYZ_RELOC_ABS32       2u

/* 符号名与模块名上限（含结尾 0） */
#define SLYZ_SYMBOL_NAME_MAX   64u
#define SLYZ_MODULE_NAME_MAX   32u

#pragma pack(push, 1)

struct SlyzModuleHeader {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t flags;
    uint32_t header_size;
    uint32_t load_addr;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t entry_offset;
    uint32_t image_off;        /* 镜像在文件中的偏移 */
    uint32_t export_off;
    uint32_t export_count;
    uint32_t import_off;
    uint32_t import_count;
    uint32_t reloc_off;
    uint32_t reloc_count;
    uint32_t dep_off;
    uint32_t dep_count;
    uint32_t checksum;
    char     name[SLYZ_MODULE_NAME_MAX];
};

struct SlyzSymbol {
    char     name[SLYZ_SYMBOL_NAME_MAX];
    uint32_t offset;
    uint16_t kind;
    uint16_t version;
    uint32_t reserved;
};

struct SlyzImport {
    char     name[SLYZ_SYMBOL_NAME_MAX];
    uint32_t slot_offset;
    uint16_t kind;
    uint16_t version;
    uint32_t reserved;
};

struct SlyzReloc {
    uint32_t offset;
    uint8_t  type;
    uint8_t  reserved[3];
};

struct SlyzDep {
    char     name[SLYZ_MODULE_NAME_MAX];
    uint16_t min_version;
    uint16_t flags;
};

#pragma pack(pop)

#endif /* SLYZLINK_H */
