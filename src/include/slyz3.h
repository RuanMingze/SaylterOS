/* ============================================================
 * slyz3.h - SLYZ v3 寄存器字节码与运行时 ABI
 * ------------------------------------------------------------
 * v3 与 v2 并存：内核同时保留栈式解释器（执行旧 .slyz）和本
 * 文件的寄存器解释器（执行新镜像）。镜像头部 magic 不同，
 * 加载器据此选择解释器，旧应用不需要重新编译。
 *
 * 设计要点：
 *   1. 定长 8 字节指令，取指与跳转目标校验都只做一次边界检查。
 *   2. 每帧固定 REGS_PER_FRAME 个寄存器，调用时复制参数到被调
 *      帧的 r0..rN-1，返回时把 r0 写回调用者约定的基址寄存器。
 *   3. 值模型与 v2 相同（SlyzVal / SlyzObj），宿主系统调用 ABI
 *      保持 SLE_SYS_* 不变，因此窗口/文件/故障注入等宿主实现
 *      可以按同一套编号分发。
 *   4. 字符串与数组对象按引用计数回收，分配走宿主的 alloc/free，
 *      内核与 Windows 命令行宿主共享同一份语义。
 *
 * 文件布局（小端）：
 *   offset 0   u8  magic[8]      "SLYZ3BIN"
 *   offset 8   u16 version       3
 *   offset 10  u16 flags         bit0 = 使用 GUI
 *   offset 12  u32 const_size
 *   offset 16  u32 code_size
 *   offset 20  u32 entry         入口代码偏移
 *   offset 24  u32 checksum      FNV-1a(const_pool || code)
 *   offset 28  u32 globals       全局槽数量
 *   offset 32  u32 func_count
 *   offset 36  u32 regs_per_frame
 *   offset 40  u32 source_size
 *   offset 44  u32 reserved
 *   offset 48  functions[func_count] { u32 entry; u32 argc; u32 regs; u32 name_off; }
 *   之后       const_pool，再之后 code
 * ============================================================ */

#ifndef SLYZ3_H
#define SLYZ3_H

#include <stdint.h>

#include "../kernel/slyzvm.h"

#define SLYZ3_MAGIC0 'S'
#define SLYZ3_MAGIC1 'L'
#define SLYZ3_MAGIC2 'Y'
#define SLYZ3_MAGIC3 'Z'
#define SLYZ3_MAGIC4 '3'
#define SLYZ3_MAGIC5 'B'
#define SLYZ3_MAGIC6 'I'
#define SLYZ3_MAGIC7 'N'

#define SLYZ3_VERSION       3
#define SLYZ3_HEADER_SIZE   48
#define SLYZ3_FUNC_ENTRY    16
#define SLYZ3_INSN_SIZE     8

#define SLYZ3_MAX_CONST     (64 * 1024)
#define SLYZ3_MAX_CODE      (256 * 1024)
#define SLYZ3_MAX_GLOBALS   256
#define SLYZ3_MAX_FRAMES    32
#define SLYZ3_REGS_PER_FRAME 64
#define SLYZ3_MAX_FUNCS     256
#define SLYZ3_MAX_STEPS     200000000u

/* 头部字段偏移 */
#define SLYZ3_HDR_VERSION   8
#define SLYZ3_HDR_FLAGS     10
#define SLYZ3_HDR_CONSTSIZE 12
#define SLYZ3_HDR_CODESIZE  16
#define SLYZ3_HDR_ENTRY     20
#define SLYZ3_HDR_CHECKSUM  24
#define SLYZ3_HDR_GLOBALS   28
#define SLYZ3_HDR_FUNCS     32
#define SLYZ3_HDR_REGS      36
#define SLYZ3_HDR_SRCSIZE   40
#define SLYZ3_HDR_RESERVED  44

#define SLYZ3_FLAG_GUI      0x0001u

/* ---------------- 操作码 ----------------
 * 指令编码：u8 op, u8 a, u8 b, u8 c, u32 imm（固定 8 字节）。
 * 寄存器编号 0..63；任何写寄存器的指令都会释放目标寄存器里
 * 原有的字符串/数组引用，避免长循环里泄漏。 */

enum {
    S3_NOP = 0,
    S3_MOVE,        /* a=rd, b=rs                  rd = rs              */
    S3_LOADI,       /* a=rd, imm                   rd = (int64)imm      */
    S3_LOADNIL,     /* a=rd                        rd = nil             */
    S3_LOADK,       /* a=rd, imm=常量偏移           rd = 常量字符串      */
    S3_LOADG,       /* a=rd, imm=全局槽             rd = globals[slot]   */
    S3_STOREG,      /* a=rs, imm=全局槽             globals[slot] = rs   */
    S3_ADD,         /* a=rd, b=ra, c=rb             rd = ra + rb         */
    S3_SUB,
    S3_MUL,
    S3_DIV,
    S3_MOD,
    S3_NEG,         /* a=rd, b=ra                  rd = -ra             */
    S3_NOT,         /* a=rd, b=ra                  rd = !ra             */
    S3_EQ,
    S3_NE,
    S3_LT,
    S3_LE,
    S3_GT,
    S3_GE,
    S3_JMP,         /* imm=绝对代码偏移                                  */
    S3_JZ,          /* a=cond, imm=目标，条件为假时跳转                    */
    S3_JNZ,         /* a=cond, imm=目标，条件为真时跳转                    */
    S3_CALL,        /* a=函数下标, b=参数基址, imm=参数个数               */
    S3_RET,         /* a=返回值寄存器                                     */
    S3_NEWARR,      /* a=rd, b=元素基址, imm=元素个数                     */
    S3_IDXGET,      /* a=rd, b=数组, c=下标                               */
    S3_IDXSET,      /* a=数组, b=下标, c=值                               */
    S3_LEN,         /* a=rd, b=对象                                       */
    S3_TYPEOF,      /* a=rd, b=对象                                       */
    S3_SYS,         /* a=rd, b=参数基址, imm=(id<<16)|argc                */
    S3_APPEND,      /* a=数组, b=值                                       */
    S3_POPARR,      /* a=rd, b=数组                                       */
    S3_RANGE,       /* a=rd, b=起始寄存器(lo,hi 两个连续寄存器)            */
    S3_HALT,
    /* 位运算：新增在末尾，保证旧镜像的操作码编号不变。
     * SlyzFS v2 用它们做 FAT 掩码、簇号拆分与扇区地址计算。 */
    S3_BAND,        /* a=rd, b=ra, c=rb             rd = ra & rb         */
    S3_BOR,         /* a=rd, b=ra, c=rb             rd = ra | rb         */
    S3_BXOR,        /* a=rd, b=ra, c=rb             rd = ra ^ rb         */
    S3_SHL,         /* a=rd, b=ra, c=rb             rd = ra << rb        */
    S3_SHR          /* a=rd, b=ra, c=rb             rd = (u64)ra >> rb   */
};

/* ---------------- 宿主回调 ---------------- */

struct Slyz3Vm;

typedef struct Slyz3Ops {
    /* 与 v2 相同的系统调用编号和参数顺序。返回值构造使用
     * slyz3_ret_* / slyz3_arr_set，不能直接写 SlyzVal。 */
    int  (*sys)(struct Slyz3Vm *vm, uint16_t id,
                struct SlyzVal *args, uint16_t argc,
                struct SlyzVal *ret);

    void *(*alloc)(uint32_t bytes);
    void  (*free)(void *ptr);
    void  (*tick)(void);
    void  (*log)(const char *text);
} Slyz3Ops;

/* ---------------- 生命周期 ---------------- */

int  slyz3_is_binary(const uint8_t *data, int size);

struct Slyz3Vm *slyz3vm_create(const uint8_t *image, int size,
                               const struct Slyz3Ops *ops, const char *name);
void slyz3vm_destroy(struct Slyz3Vm *vm);

/* 返回 1 仍在运行，0 正常结束，<0 运行错误 */
int  slyz3vm_step(struct Slyz3Vm *vm, uint32_t budget);
int  slyz3vm_run(const uint8_t *image, int size,
                 const struct Slyz3Ops *ops, const char *name);

int  slyz3vm_state(const struct Slyz3Vm *vm);
int  slyz3vm_exit_code(const struct Slyz3Vm *vm);
const char *slyz3vm_error(const struct Slyz3Vm *vm);
uint32_t slyz3vm_steps(const struct Slyz3Vm *vm);
int  slyz3vm_uses_gui(const struct Slyz3Vm *vm);

void slyz3vm_request_exit(struct Slyz3Vm *vm, int code);
void slyz3vm_request_yield(struct Slyz3Vm *vm);

/* ---------------- 返回值构造（宿主使用） ---------------- */

void slyz3_ret_nil(struct SlyzVal *ret);
void slyz3_ret_int(struct SlyzVal *ret, int64_t x);
int  slyz3_ret_str(struct Slyz3Vm *vm, struct SlyzVal *ret, const char *s, int len);
int  slyz3_ret_arr(struct Slyz3Vm *vm, struct SlyzVal *ret, int count);
int  slyz3_arr_set(struct Slyz3Vm *vm, struct SlyzVal *arr, int index,
                   struct SlyzVal value);

#endif /* SLYZ3_H */
