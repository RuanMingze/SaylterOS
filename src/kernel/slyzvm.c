/* ============================================================
 * slyzvm.c - SLYZ 二进制可执行格式的解释器
 * ------------------------------------------------------------
 * 只做三件事：校验文件头、在常量池里取字符串、按字节码跳转执行。
 * 所有 I/O 通过 SlyzVmOps 回调交给 shell，因此本文件不依赖 tty。
 * ============================================================ */

#include "slyzvm.h"
#include "tty.h"

/* ---------- 小端读取 ---------- */

static uint16_t vm_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t vm_rd32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t vm_fnv1a(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

static int vm_strlen_limited(const char *s, int max) {
    int n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int slyzvm_is_binary(const uint8_t *data, int size) {
    if (!data || size < SLYZVM_HEADER_SIZE) return 0;
    return data[0] == SLYZVM_MAGIC0 && data[1] == SLYZVM_MAGIC1 &&
           data[2] == SLYZVM_MAGIC2 && data[3] == SLYZVM_MAGIC3 &&
           data[4] == SLYZVM_MAGIC4 && data[5] == SLYZVM_MAGIC5 &&
           data[6] == SLYZVM_MAGIC6 && data[7] == SLYZVM_MAGIC7;
}

/* 常量池里的字符串必须完整落在 const_size 内 */
static const char *vm_const_at(const uint8_t *pool, int const_size, uint16_t offset) {
    if (offset >= (uint16_t)const_size) return 0;
    int max = const_size - (int)offset;
    int len = vm_strlen_limited((const char *)(pool + offset), max);
    if (len >= max) return 0;   /* 没有结尾 '\0' */
    return (const char *)(pool + offset);
}

int slyzvm_run(const uint8_t *data, int size,
               const struct SlyzVmOps *ops, const char *name) {
    if (!data || !ops || !slyzvm_is_binary(data, size)) return 0;

    uint16_t version = vm_rd16(data + 8);
    uint32_t const_size = vm_rd32(data + 12);
    uint32_t code_size = vm_rd32(data + 16);
    uint32_t entry = vm_rd32(data + 20);
    uint32_t checksum = vm_rd32(data + 24);

    if (version != SLYZVM_VERSION) {
        printk("%s: unsupported SLYZ version %u\n", name, (unsigned int)version);
        return 0;
    }
    if (const_size > SLYZVM_MAX_CONST || code_size > SLYZVM_MAX_CODE) {
        printk("%s: image too large\n", name);
        return 0;
    }
    if ((uint32_t)size < SLYZVM_HEADER_SIZE + const_size + code_size) {
        printk("%s: truncated image\n", name);
        return 0;
    }
    if (entry >= code_size) {
        printk("%s: bad entry point\n", name);
        return 0;
    }

    const uint8_t *pool = data + SLYZVM_HEADER_SIZE;
    const uint8_t *code = pool + const_size;

    if (vm_fnv1a(code - const_size, (int)(const_size + code_size)) != checksum) {
        printk("%s: checksum mismatch (image corrupted)\n", name);
        return 0;
    }

    uint32_t pc = entry;
    int last = 0;
    uint32_t steps = 0;

    /* ALLOC 的返回指针按栈式记录，FREE 释放最近一次。
     * 最多同时持有 8 块，超出时报错终止脚本，避免内存泄漏。 */
    void *allocs[8];
    int alloc_count = 0;

    while (pc < code_size) {
        if (++steps > SLYZVM_MAX_STEPS) {
            printk("%s: runaway script aborted\n", name);
            return 0;
        }

        if (ops->tick) ops->tick();

        uint8_t op = code[pc++];

        switch (op) {
            case SLOP_END:
                return 1;

            case SLOP_NOP:
                break;

            case SLOP_ECHO: {
                if (pc + 2 > code_size) goto bad_image;
                uint16_t off = vm_rd16(code + pc);
                pc += 2;
                const char *s = vm_const_at(pool, (int)const_size, off);
                if (!s) goto bad_image;
                ops->print(s);
                break;
            }

            case SLOP_ASK: {
                if (pc + 2 > code_size) goto bad_image;
                uint16_t off = vm_rd16(code + pc);
                pc += 2;
                const char *s = vm_const_at(pool, (int)const_size, off);
                if (!s) goto bad_image;
                last = ops->ask(s) ? 1 : 0;
                break;
            }

            case SLOP_RUN: {
                if (pc + 2 > code_size) goto bad_image;
                uint16_t off = vm_rd16(code + pc);
                pc += 2;
                const char *s = vm_const_at(pool, (int)const_size, off);
                if (!s) goto bad_image;
                if (!ops->run_line(s)) {
                    printk("%s: command failed: %s\n", name, s);
                    return 0;
                }
                break;
            }

            case SLOP_JMP: {
                if (pc + 2 > code_size) goto bad_image;
                pc = vm_rd16(code + pc);
                break;
            }

            case SLOP_JZ: {
                if (pc + 2 > code_size) goto bad_image;
                uint16_t target = vm_rd16(code + pc);
                pc += 2;
                if (last == 0) pc = target;
                break;
            }

            case SLOP_JNZ: {
                if (pc + 2 > code_size) goto bad_image;
                uint16_t target = vm_rd16(code + pc);
                pc += 2;
                if (last != 0) pc = target;
                break;
            }

            case SLOP_EXIT:
                return 1;

            case SLOP_HELP:
                ops->help();
                break;

            case SLOP_CLEAR:
                ops->clear();
                break;

            case SLOP_SET: {
                if (pc + 1 > code_size) goto bad_image;
                last = code[pc++] ? 1 : 0;
                break;
            }

            case SLOP_ALLOC: {
                if (pc + 2 > code_size) goto bad_image;
                uint16_t bytes = vm_rd16(code + pc);
                pc += 2;

                if (alloc_count >= 8) {
                    printk("%s: too many live allocations (max 8)\n", name);
                    return 0;
                }
                if (bytes == 0 || !ops->alloc) {
                    printk("%s: cannot allocate %u bytes\n", name, (unsigned int)bytes);
                    return 0;
                }

                void *ptr = ops->alloc(bytes);
                if (!ptr) {
                    /* 配额或物理内存不足：由宿主决定是否已经杀掉任务 */
                    printk("%s: allocation of %u bytes failed\n",
                           name, (unsigned int)bytes);
                    return 0;
                }
                allocs[alloc_count++] = ptr;
                break;
            }

            case SLOP_FREE: {
                if (alloc_count <= 0) {
                    printk("%s: free without matching alloc\n", name);
                    return 0;
                }
                alloc_count--;
                if (ops->free) ops->free(allocs[alloc_count]);
                allocs[alloc_count] = 0;
                break;
            }

            case SLOP_WAIT: {
                /* 停在 WAIT 上反复让出 CPU。任务被 kill 后调度器不再
                 * 选择它，栈就永久冻结在这里，等价于进程被终止。 */
                pc--;
                if (ops->yield) {
                    ops->yield();
                } else {
                    return 1;
                }
                break;
            }

            default:
                printk("%s: unknown opcode 0x%x at %u\n",
                       name, (unsigned int)op, (unsigned int)(pc - 1));
                return 0;
        }

        if (pc > code_size) goto bad_image;
    }

    return 1;

bad_image:
    printk("%s: malformed bytecode\n", name);
    return 0;
}
