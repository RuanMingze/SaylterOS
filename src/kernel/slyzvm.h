/* ============================================================
 * slyzvm.h - SLYZ 二进制可执行格式（SLYZBIN v1）
 * ------------------------------------------------------------
 * 设计目标：
 *   1. 与文本 .slyz 彻底区分：文件以 8 字节魔数 "SLYZBIN\0" 开头，
 *      任何文本文件都不会误判。
 *   2. 位置无关：只有绝对偏移，没有重定位表，读进内存即可执行，
 *      因此 Windows 上编译的产物可以直接放进 SlyzFS 交给内核。
 *   3. 结构极简：32 字节头 + 常量池 + 字节码，全部小端。
 *
 * 文件布局：
 *   offset 0   magic[8]        "SLYZBIN\0"
 *   offset 8   u16 version     当前为 1
 *   offset 10  u16 flags       bit0 = 需要交互输入
 *   offset 12  u32 const_size  常量池字节数
 *   offset 16  u32 code_size   字节码字节数
 *   offset 20  u32 entry       入口偏移（通常 0）
 *   offset 24  u32 checksum    FNV-1a(const_pool || code)
 *   offset 28  u32 reserved    必须为 0
 *   offset 32  const_pool      以 '\0' 分隔的字符串常量池
 *   之后       code            字节码
 *
 * 字节码指令（u16 操作数为小端绝对偏移）：
 *   0x00 END
 *   0x01 ECHO   u16 str
 *   0x02 ASK    u16 str
 *   0x03 JMP    u16 target
 *   0x04 JZ     u16 target      last == 0 时跳转
 *   0x05 JNZ    u16 target      last != 0 时跳转
 *   0x06 RUN    u16 str         交给宿主执行一行命令
 *   0x07 EXIT
 *   0x08 HELP
 *   0x09 CLEAR
 *   0x0A SET    u8 value        写 last
 *   0x0B NOP
 *   0x0C ALLOC  u16 bytes       向宿主申请内存
 *   0x0D FREE                   释放最近一次 ALLOC
 *   0x0E WAIT                   永久让出 CPU，直到任务被 kill
 * ============================================================ */

#ifndef SLYZVM_H
#define SLYZVM_H

#include <stdint.h>

#define SLYZVM_MAGIC0   'S'
#define SLYZVM_MAGIC1   'L'
#define SLYZVM_MAGIC2   'Y'
#define SLYZVM_MAGIC3   'Z'
#define SLYZVM_MAGIC4   'B'
#define SLYZVM_MAGIC5   'I'
#define SLYZVM_MAGIC6   'N'
#define SLYZVM_MAGIC7   0x00

#define SLYZVM_VERSION        1
#define SLYZVM_HEADER_SIZE    32

/* 单文件上限：常量池与字节码各不超过 4KB，合计 8KB */
#define SLYZVM_MAX_CONST      4096
#define SLYZVM_MAX_CODE       4096
#define SLYZVM_MAX_FILE       (SLYZVM_HEADER_SIZE + SLYZVM_MAX_CONST + SLYZVM_MAX_CODE)

/* 单次执行的最大指令数，防止损坏文件导致死循环 */
#define SLYZVM_MAX_STEPS      200000

/* 指令操作码 */
#define SLOP_END     0x00
#define SLOP_ECHO    0x01
#define SLOP_ASK     0x02
#define SLOP_JMP     0x03
#define SLOP_JZ      0x04
#define SLOP_JNZ     0x05
#define SLOP_RUN     0x06
#define SLOP_EXIT    0x07
#define SLOP_HELP    0x08
#define SLOP_CLEAR   0x09
#define SLOP_SET     0x0A
#define SLOP_NOP     0x0B
#define SLOP_ALLOC   0x0C
#define SLOP_FREE    0x0D
#define SLOP_WAIT    0x0E

/* 宿主回调：VM 自身不做任何 I/O，全部由 shell 注入 */
struct SlyzVmOps {
    void (*print)(const char *text);
    int  (*ask)(const char *prompt);       /* 返回 0 或 1 */
    int  (*run_line)(const char *line);    /* 执行一行 shell 命令，成功返回 1 */
    void (*clear)(void);
    void (*help)(void);
    /* 内存申请：返回 0 表示失败（配额或物理内存不足），VM 会终止脚本 */
    void *(*alloc)(uint32_t bytes);
    void  (*free)(void *ptr);
    /* 每执行一条指令调用一次，用于任务步数统计与协作式调度；可为 0 */
    void (*tick)(void);
    /* 主动让出 CPU（WAIT 指令每次调用）；可为 0 */
    void (*yield)(void);
};

/* 只检查 8 字节魔数，用于区分二进制与文本脚本 */
int slyzvm_is_binary(const uint8_t *data, int size);

/* 校验并执行。成功返回 1；格式错误/校验失败/执行出错返回 0。 */
int slyzvm_run(const uint8_t *data, int size,
               const struct SlyzVmOps *ops, const char *name);

#endif /* SLYZVM_H */
