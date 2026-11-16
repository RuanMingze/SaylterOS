/* ============================================================
 * slyz.h - SLYZ 语言 v2 二进制格式与虚拟机指令集
 * ------------------------------------------------------------
 * 本头文件是编译器（tools/slyzc）、宿主运行时与内核虚拟机
 * （src/kernel/slyzvm.c）之间唯一的格式约定。任何一侧修改
 * 操作码或头部布局，都必须同步另一边，否则镜像会被拒绝执行。
 *
 * 设计原则：
 *   1. 位置无关：只有绝对偏移，读进内存即可执行，无重定位表。
 *   2. 定长操作数：所有多字节操作数固定宽度，便于逐条校验，
 *      宿主反汇编与内核解释器共用同一套解码逻辑。
 *   3. 显式校验：头部给出常量池与代码长度，FNV-1a 覆盖两者，
 *      任何截断或篡改都会在执行前被拒绝。
 *
 * 文件布局（全部小端）：
 *   offset 0   u8  magic[8]      "SLYZBIN\0"
 *   offset 8   u16 version       2
 *   offset 10  u16 flags         bit0 = 使用 GUI/窗口调用
 *   offset 12  u32 const_size    常量池字节数
 *   offset 16  u32 code_size     字节码字节数
 *   offset 20  u32 entry         入口在代码段内的偏移
 *   offset 24  u32 checksum      FNV-1a(const_pool || code)
 *   offset 28  u32 globals       全局变量槽数量
 *   offset 32  u32 stack_hint    建议值栈深度（编译期最大深度）
 *   offset 36  u32 source_size   源码字节数（仅用于诊断，可为 0）
 *   offset 40  u32 reserved0     必须为 0
 *   offset 44  u32 reserved1     必须为 0
 *   offset 48  const_pool        以 '\0' 结尾的字符串依次排列
 *   之后       code              字节码
 *
 * 值模型：
 *   运行时每个值占 8 字节载荷 + 1 字节类型标记（实现内部按
 *   结构体对齐）。整数为有符号 64 位；字符串与数组是引用计数
 *   堆对象；nil 表示"没有值"。
 * ============================================================ */

#ifndef SLYZ_FORMAT_H
#define SLYZ_FORMAT_H

#include <stdint.h>

/* ---------- 魔数与版本 ---------- */

#define SLYZ_MAGIC0 'S'
#define SLYZ_MAGIC1 'L'
#define SLYZ_MAGIC2 'Y'
#define SLYZ_MAGIC3 'Z'
#define SLYZ_MAGIC4 'B'
#define SLYZ_MAGIC5 'I'
#define SLYZ_MAGIC6 'N'
#define SLYZ_MAGIC7 0x00

#define SLYZ_VERSION       2
#define SLYZ_HEADER_SIZE   48

/* ---------- 头部字段偏移 ---------- */

#define SLYZ_HDR_VERSION   8
#define SLYZ_HDR_FLAGS     10
#define SLYZ_HDR_CONSTSIZE 12
#define SLYZ_HDR_CODESIZE  16
#define SLYZ_HDR_ENTRY     20
#define SLYZ_HDR_CHECKSUM  24
#define SLYZ_HDR_GLOBALS   28
#define SLYZ_HDR_STACKHINT 32
#define SLYZ_HDR_SRCSIZE   36
#define SLYZ_HDR_RESERVED0 40
#define SLYZ_HDR_RESERVED1 44

/* flags 位定义 */
#define SLYZ_FLAG_GUI      0x0001u

/* ---------- 值类型标记 ---------- */

#define SLYZ_T_NIL  0
#define SLYZ_T_INT  1
#define SLYZ_T_STR  2
#define SLYZ_T_ARR  3

/* ---------- 虚拟机限制 ----------
 * 常量池 64KB、代码 256KB 足够写出完整 GUI 应用，同时保证
 * 载入镜像时能用一次连续分配完成，不会在裸机上产生碎片。
 * 栈深 2048 个值 = 16KB 工作区，配合默认 64KB 任务配额。 */

#define SLYZ_MAX_CONST      (64 * 1024)
#define SLYZ_MAX_CODE       (256 * 1024)
#define SLYZ_MAX_FILE       (SLYZ_HEADER_SIZE + SLYZ_MAX_CONST + SLYZ_MAX_CODE)
#define SLYZ_MAX_GLOBALS    256
#define SLYZ_MAX_FRAMES     64
#define SLYZ_MAX_STACK      2048
#define SLYZ_MAX_STEPS      200000000u
#define SLYZ_HEAP_MIN       (16 * 1024)

/* ---------- 操作码 ----------
 * 无操作数的算术与栈操作占 0x00..0x1F；
 * 带 u32 操作数的跳转/变量/调用占 0x20..0x2F；
 * 系统调用占 0x30。
 *
 * 栈效果写作 (输入 -> 输出)。 */

#define SLE_OP_HALT       0x00u   /* () -> 结束执行 */
#define SLE_OP_NOP        0x01u
#define SLE_OP_POP        0x02u   /* (a) -> () */
#define SLE_OP_DUP        0x03u   /* (a) -> (a a) */
#define SLE_OP_SWAP       0x04u   /* (a b) -> (b a) */
#define SLE_OP_ADD        0x05u   /* (a b) -> (a+b) */
#define SLE_OP_SUB        0x06u
#define SLE_OP_MUL        0x07u
#define SLE_OP_DIV        0x08u
#define SLE_OP_MOD        0x09u
#define SLE_OP_NEG        0x0Au   /* (a) -> (-a) */
#define SLE_OP_EQ         0x0Bu
#define SLE_OP_NE         0x0Cu
#define SLE_OP_LT         0x0Du
#define SLE_OP_LE         0x0Eu
#define SLE_OP_GT         0x0Fu
#define SLE_OP_GE         0x10u
#define SLE_OP_AND        0x11u
#define SLE_OP_OR         0x12u
#define SLE_OP_NOT        0x13u
#define SLE_OP_RET        0x14u   /* (ret) -> 返回到调用者 */
#define SLE_OP_PUSH_NIL   0x15u
#define SLE_OP_PUSH_TRUE  0x16u
#define SLE_OP_PUSH_FALSE 0x17u
#define SLE_OP_IDX_GET    0x18u   /* (obj idx) -> value */
#define SLE_OP_IDX_SET    0x19u   /* (obj idx value) -> () */
#define SLE_OP_TYPEOF     0x1Au   /* (a) -> int */
#define SLE_OP_DUP2       0x1Bu   /* (a b) -> (a b a b) */

#define SLE_OP_PUSH_I     0x20u   /* u64 */
#define SLE_OP_PUSH_S     0x21u   /* u32 常量池偏移 */
#define SLE_OP_LOAD_L     0x22u   /* u32 帧内槽号 */
#define SLE_OP_STORE_L    0x23u   /* u32 帧内槽号 */
#define SLE_OP_LOAD_G     0x24u   /* u32 全局槽号 */
#define SLE_OP_STORE_G    0x25u   /* u32 全局槽号 */
#define SLE_OP_JMP        0x26u   /* u32 绝对代码偏移 */
#define SLE_OP_JZ         0x27u   /* u32 目标，弹出栈顶 */
#define SLE_OP_JNZ        0x28u   /* u32 目标，弹出栈顶 */
#define SLE_OP_CALL       0x29u   /* u32 入口 + u32 参数个数 */
#define SLE_OP_MAKE_ARR   0x2Au   /* u32 元素个数，依次弹出构造数组 */
#define SLE_OP_SYS        0x30u   /* u16 调用号 + u16 参数个数 */

/* ---------- 宿主系统调用号（< 100） ----------
 * 这些调用必须由宿主注入的实现完成，虚拟机只负责搬运参数。
 * 参数按源码书写顺序压栈，最后一个参数在栈顶。 */

#define SLE_SYS_PRINT       0u   /* (v)            打印值，不换行 */
#define SLE_SYS_PRINTLN     1u   /* (v)            打印值并换行 */
#define SLE_SYS_EPRINT      2u   /* (v)            打印到错误输出 */

#define SLE_SYS_FS_READ     20u  /* (path)         读文本，失败返回 nil */
#define SLE_SYS_FS_WRITE    21u  /* (path, text)   写入，返回字节数或 -1 */
#define SLE_SYS_FS_EXISTS   22u  /* (path)         0/1 */
#define SLE_SYS_FS_LIST     23u  /* (path)         目录项名字数组 */
#define SLE_SYS_FS_REMOVE   24u  /* (path)         0 成功，-1 失败 */
#define SLE_SYS_FS_MKDIR    25u  /* (path)         0 成功，-1 失败 */
#define SLE_SYS_FS_ENTRIES  26u  /* (path)         [[name, type, size], ...] */

#define SLE_SYS_CFG_GET     30u  /* (key)          etc\system.cfg 的值 */
#define SLE_SYS_CFG_SET     31u  /* (key, value)   写回配置，0 成功 */
#define SLE_SYS_CFG_KEYS    32u  /* ()             全部键名数组 */

#define SLE_SYS_TIME_NOW    40u  /* ()             Unix 秒 */
#define SLE_SYS_TIME_MS     41u  /* ()             开机毫秒 */
#define SLE_SYS_SLEEP       42u  /* (ms) */
#define SLE_SYS_BEEP        43u  /* (freq, ms) */
#define SLE_SYS_TIME_TEXT   44u  /* ()             "HH:MM:SS" */
#define SLE_SYS_DATE_TEXT   45u  /* ()             "YYYY-MM-DD" */

#define SLE_SYS_EXEC        50u  /* (cmdline)      shell 执行，返回退出码 */
#define SLE_SYS_EXIT        51u  /* (code)         立即结束程序 */

#define SLE_SYS_WIN_OPEN    60u  /* (title, w, h)  创建窗口，返回句柄 */
#define SLE_SYS_WIN_CLOSE   61u  /* (handle) */
#define SLE_SYS_WIN_SIZE    62u  /* (handle)       宽，高度另取 */
#define SLE_SYS_WIN_HEIGHT  63u  /* (handle) */
#define SLE_SYS_WIN_TITLE   64u  /* (handle, title) */

#define SLE_SYS_GFX_CLEAR   70u  /* (color) */
#define SLE_SYS_GFX_RECT    71u  /* (x, y, w, h, color) */
#define SLE_SYS_GFX_FRAME   72u  /* (x, y, w, h, color, thickness) */
#define SLE_SYS_GFX_TEXT    73u  /* (x, y, text, color) */
#define SLE_SYS_GFX_TWIDTH  74u  /* (text)         像素宽度 */
#define SLE_SYS_GFX_LINE    75u  /* (x1, y1, x2, y2, color) */
#define SLE_SYS_GFX_CIRCLE  76u  /* (cx, cy, r, color) */
#define SLE_SYS_GFX_FLUSH   77u  /* ()             提交本帧绘制 */

#define SLE_SYS_EVT_POLL    80u  /* ()              事件数组或 nil */
#define SLE_SYS_EVT_WAIT    81u  /* (timeout_ms)    同上，阻塞等待 */

/* 桌面集成与音频播放 */
#define SLE_SYS_OPEN_UI     78u  /* (path)          按扩展名打开（媒体/编辑器/应用） */
#define SLE_SYS_MEDIA_REQ   79u  /* ()              取走双击 MP3 时设置的待播放路径 */
#define SLE_SYS_AUDIO_PLAY  82u  /* (path)          开始播放 MP3，0 成功 */
#define SLE_SYS_AUDIO_STOP  83u  /* ()              停止播放 */
#define SLE_SYS_AUDIO_PAUSE 84u  /* (paused)        暂停/继续 */
#define SLE_SYS_AUDIO_STATE 85u  /* ()              0 停止 1 播放 2 暂停 */
#define SLE_SYS_AUDIO_POS   86u  /* ()              当前毫秒 */
#define SLE_SYS_AUDIO_DUR   87u  /* ()              总毫秒 */
#define SLE_SYS_AUDIO_VOL   88u  /* (volume)        设置音量 0..100 */
#define SLE_SYS_AUDIO_GETVOL 89u /* ()              读取音量 0..100 */

/* 系统信息查询（任务管理器使用）。key 见下方 SLE_INFO_*。 */
#define SLE_SYS_INFO        90u  /* (key)           -> int */
#define SLE_SYS_PROC        91u  /* (index, field)  -> int，非法返回 -1 */
#define SLE_SYS_PROC_NAME   92u  /* (index)         -> str，非法返回 "" */

/* 图片（BMP）：句柄是宿主侧的缓存下标，非法/加载失败返回 -1。 */
#define SLE_SYS_IMG_LOAD         93u  /* (path)                -> handle */
#define SLE_SYS_IMG_WIDTH        94u  /* (handle)              -> int */
#define SLE_SYS_IMG_HEIGHT       95u  /* (handle)              -> int */
#define SLE_SYS_IMG_DRAW         96u  /* (handle, x, y)        */
#define SLE_SYS_IMG_DRAW_SCALED  97u  /* (handle, x, y, w, h)  */
#define SLE_SYS_IMG_DRAW_CIRCLE  98u  /* (handle, cx, cy, r)   */
#define SLE_SYS_IMG_FREE         99u  /* (handle)              */

/* SLE_SYS_INFO 的 key */
#define SLE_INFO_MEM_TOTAL     1
#define SLE_INFO_MEM_USED      2
#define SLE_INFO_MEM_PEAK      3
#define SLE_INFO_MEM_FREE      4
#define SLE_INFO_TASKS_ALIVE   5
#define SLE_INFO_TASK_MEM      6
#define SLE_INFO_DISK_WRITTEN  7
#define SLE_INFO_UPTIME_MS     8
#define SLE_INFO_CPU_PERMILLE  9
#define SLE_INFO_TASK_SLOTS   10

/* SLE_SYS_PROC 的 field */
#define SLE_PROC_ID        1
#define SLE_PROC_STATE     2
#define SLE_PROC_STEPS     3
#define SLE_PROC_QUOTA     4
#define SLE_PROC_USED      5
#define SLE_PROC_EXIT      6

#define TASK_STATE_FREE    0
#define TASK_STATE_READY   1
#define TASK_STATE_RUNNING 2
#define TASK_STATE_DEAD    3

/* 事件类型（事件数组第 0 个元素） */
#define SLE_EVT_NONE      0
#define SLE_EVT_KEY       1   /* [1, 字符码, 扫描码, 0] */
#define SLE_EVT_MOUSE     2   /* [2, x, y, buttons] */
#define SLE_EVT_CLICK     3   /* [3, x, y, button] */
#define SLE_EVT_WHEEL     4   /* [4, delta, 0, 0] */
#define SLE_EVT_CLOSE     5   /* [5, 0, 0, 0] */
#define SLE_EVT_TICK      6   /* [6, ms, 0, 0] */
#define SLE_EVT_RESIZE    7   /* [7, w, h, 0] */

/* ---------- 内置函数调用号（>= 100，虚拟机内部实现） ---------- */

#define SLE_BIF_LEN        100u
#define SLE_BIF_STR        101u
#define SLE_BIF_INT        102u
#define SLE_BIF_SUBSTR     103u
#define SLE_BIF_INDEXOF    104u
#define SLE_BIF_UPPER      105u
#define SLE_BIF_LOWER      106u
#define SLE_BIF_ABS        107u
#define SLE_BIF_MIN        108u
#define SLE_BIF_MAX        109u
#define SLE_BIF_ARRAY      110u
#define SLE_BIF_PUSH       111u
#define SLE_BIF_POP        112u
#define SLE_BIF_RANGE      113u
#define SLE_BIF_RANDOM     114u
#define SLE_BIF_TYPEOF     115u
#define SLE_BIF_TRIM       116u
#define SLE_BIF_REPLACE    117u
#define SLE_BIF_SPLIT      118u
#define SLE_BIF_JOIN       119u
#define SLE_BIF_STR_AT     120u
#define SLE_BIF_ORD        121u
#define SLE_BIF_CHR        122u
#define SLE_BIF_CLAMP      123u
#define SLE_BIF_SIGN       124u
#define SLE_BIF_HEX        125u
#define SLE_BIF_BIN        126u
#define SLE_BIF_COUNT      127u

/* ---------- 执行结果 ---------- */

#define SLYZ_OK            0
#define SLYZ_ERR_FORMAT   -1
#define SLYZ_ERR_TRUNC    -2
#define SLYZ_ERR_CHECKSUM -3
#define SLYZ_ERR_VERSION  -4
#define SLYZ_ERR_ENTRY    -5
#define SLYZ_ERR_STACK    -6
#define SLYZ_ERR_TYPE     -7
#define SLYZ_ERR_OOM      -8
#define SLYZ_ERR_SYS      -9
#define SLYZ_ERR_STEPS    -10
#define SLYZ_ERR_CALL     -11

#endif /* SLYZ_FORMAT_H */
