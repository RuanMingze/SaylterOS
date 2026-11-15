/*
 * slyzc - SLYZ 二进制编译器（Windows 版）
 * ------------------------------------------------------------
 * 把文本 .slyz 源文件编译成 SaylterOS 内核可直接执行的
 * SLYZBIN v1 字节码镜像。产物与内核 slyzvm.c 完全同构：
 *
 *   32 字节头 + 常量池 + 字节码
 *
 * 用法：
 *   slyztool compile <input.slyz> [-o output.bin]
 *   slyztool check   <input.slyz>
 *   slyztool dump    <input.bin>
 *   slyztool run     <input.slyz>
 *   slyztool doc
 *
 * 编译：
 *   gcc -std=c11 -O2 -Wall -Wextra -o slyztool.exe slyz_tool.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ============================================================
 * 与内核 slyzvm.h 保持一致的格式常量
 * ============================================================ */

#define SLYZ_MAGIC          "SLYZBIN"
#define SLYZ_HEADER_SIZE    32
#define SLYZ_VERSION        1

#define SLYZ_MAX_CONST      4096
#define SLYZ_MAX_CODE       4096
#define SLYZ_MAX_FILE       (SLYZ_HEADER_SIZE + SLYZ_MAX_CONST + SLYZ_MAX_CODE)
#define SLYZ_MAX_STEPS      200000

#define SLOP_END            0x00
#define SLOP_ECHO           0x01
#define SLOP_ASK            0x02
#define SLOP_JMP            0x03
#define SLOP_JZ             0x04
#define SLOP_JNZ            0x05
#define SLOP_RUN            0x06
#define SLOP_EXIT           0x07
#define SLOP_HELP           0x08
#define SLOP_CLEAR          0x09
#define SLOP_SET            0x0A
#define SLOP_NOP            0x0B

/* ============================================================
 * 编译器状态
 * ============================================================ */

static uint8_t g_code[SLYZ_MAX_CODE];
static int     g_code_len = 0;

static char    g_pool[SLYZ_MAX_CONST];
static int     g_pool_len = 0;

static int     g_errors = 0;
static int     g_line_no = 0;
static const char *g_source_path = "";

struct IfFrame {
    int jz_at;      /* JZ/JNZ 指令操作数位置，等待回填 */
    int jmp_at;     /* else 生成的无条件跳转位置，-1 表示没有 else */
};

static struct IfFrame g_ifs[32];
static int g_if_depth = 0;

/* ============================================================
 * 基础工具
 * ============================================================ */

static void error_at(int line, const char *message) {
    fprintf(stderr, "%s:%d: error: %s\n", g_source_path, line, message);
    g_errors++;
}

static char *trim_in_place(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return s;

    char *end = s + strlen(s) - 1;
    while (end >= s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end-- = '\0';
    }
    return s;
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* 从命令名后取出参数（已 trim） */
static char *command_args(char *line) {
    char *p = line;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p == '\0') return p;
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* ============================================================
 * 常量池与字节码发射
 * ============================================================ */

static int pool_add(const char *text) {
    int len = (int)strlen(text) + 1;
    if (g_pool_len + len > SLYZ_MAX_CONST) {
        error_at(g_line_no, "string pool overflow (limit 4096 bytes)");
        return -1;
    }
    int offset = g_pool_len;
    memcpy(g_pool + g_pool_len, text, (size_t)len);
    g_pool_len += len;
    return offset;
}

static void emit_byte(uint8_t value) {
    if (g_code_len >= SLYZ_MAX_CODE) {
        error_at(g_line_no, "bytecode overflow (limit 4096 bytes)");
        return;
    }
    g_code[g_code_len++] = value;
}

static int emit_u16_placeholder(void) {
    if (g_code_len + 2 > SLYZ_MAX_CODE) {
        error_at(g_line_no, "bytecode overflow (limit 4096 bytes)");
        return -1;
    }
    int at = g_code_len;
    g_code[g_code_len++] = 0;
    g_code[g_code_len++] = 0;
    return at;
}

static void emit_u16_at(int at, uint16_t value) {
    if (at < 0 || at + 1 >= SLYZ_MAX_CODE) return;
    g_code[at] = (uint8_t)(value & 0xFF);
    g_code[at + 1] = (uint8_t)((value >> 8) & 0xFF);
}

static void emit_string_op(uint8_t op, const char *text) {
    int offset = pool_add(text);
    if (offset < 0) return;
    emit_byte(op);
    emit_byte((uint8_t)(offset & 0xFF));
    emit_byte((uint8_t)((offset >> 8) & 0xFF));
}

/* ============================================================
 * 单行编译
 * ============================================================ */

static int compile_line(char *line) {
    if (line[0] == '\0') return 1;
    if (line[0] == '#') return 1;          /* 整行注释 */

    /* if yes / if no */
    if (starts_with(line, "if ")) {
        char *cond = line + 3;
        while (*cond == ' ' || *cond == '\t') cond++;

        int op;
        if (strcmp(cond, "yes") == 0 || strcmp(cond, "1") == 0) {
            op = SLOP_JZ;      /* last == 0 时跳过 then 分支 */
        } else if (strcmp(cond, "no") == 0 || strcmp(cond, "0") == 0) {
            op = SLOP_JNZ;     /* last != 0 时跳过 then 分支 */
        } else {
            error_at(g_line_no, "if expects 'yes' or 'no'");
            return 0;
        }

        if (g_if_depth >= 32) {
            error_at(g_line_no, "if nesting too deep (limit 32)");
            return 0;
        }

        emit_byte((uint8_t)op);
        int at = emit_u16_placeholder();
        if (at < 0) return 0;

        g_ifs[g_if_depth].jz_at = at;
        g_ifs[g_if_depth].jmp_at = -1;
        g_if_depth++;
        return 1;
    }

    if (strcmp(line, "else") == 0) {
        if (g_if_depth <= 0) {
            error_at(g_line_no, "'else' without matching 'if'");
            return 0;
        }

        emit_byte(SLOP_JMP);
        int jmp_at = emit_u16_placeholder();
        if (jmp_at < 0) return 0;

        /* 上一分支在这里结束，回填 if 的跳转 */
        emit_u16_at(g_ifs[g_if_depth - 1].jz_at, (uint16_t)g_code_len);
        g_ifs[g_if_depth - 1].jmp_at = jmp_at;
        return 1;
    }

    if (strcmp(line, "endif") == 0) {
        if (g_if_depth <= 0) {
            error_at(g_line_no, "'endif' without matching 'if'");
            return 0;
        }

        g_if_depth--;
        if (g_ifs[g_if_depth].jmp_at >= 0) {
            emit_u16_at(g_ifs[g_if_depth].jmp_at, (uint16_t)g_code_len);
        } else {
            emit_u16_at(g_ifs[g_if_depth].jz_at, (uint16_t)g_code_len);
        }
        return 1;
    }

    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
        emit_byte(SLOP_EXIT);
        return 1;
    }

    if (strcmp(line, "help") == 0) {
        emit_byte(SLOP_HELP);
        return 1;
    }

    if (strcmp(line, "clear") == 0 || strcmp(line, "cls") == 0) {
        emit_byte(SLOP_CLEAR);
        return 1;
    }

    /* set yes / set no：给 last 赋值，便于组合条件 */
    if (starts_with(line, "set ")) {
        char *value = line + 4;
        while (*value == ' ' || *value == '\t') value++;

        uint8_t v;
        if (strcmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
            v = 1;
        } else if (strcmp(value, "no") == 0 || strcmp(value, "0") == 0) {
            v = 0;
        } else {
            error_at(g_line_no, "set expects 'yes' or 'no'");
            return 0;
        }

        emit_byte(SLOP_SET);
        emit_byte(v);
        return 1;
    }

    if (starts_with(line, "echo")) {
        char *args = command_args(line);
        if (*args == '\0') {
            error_at(g_line_no, "echo requires text");
            return 0;
        }
        emit_string_op(SLOP_ECHO, args);
        return 1;
    }

    if (starts_with(line, "ask")) {
        char *args = command_args(line);
        if (*args == '\0') {
            error_at(g_line_no, "ask requires a prompt");
            return 0;
        }
        emit_string_op(SLOP_ASK, args);
        return 1;
    }

    if (starts_with(line, "run ")) {
        /* 保留整行，由内核交给 shell 的 run 命令处理 */
        emit_string_op(SLOP_RUN, line);
        return 1;
    }

    /* 其余整行视为 shell 命令 */
    emit_string_op(SLOP_RUN, line);
    return 1;
}

/* ============================================================
 * 源文件读取与编译
 * ============================================================ */

static char *read_entire_file(const char *path, int *size_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || size > 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    size_t got = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);
    buffer[got] = '\0';
    if (size_out) *size_out = (int)got;
    return buffer;
}

/* 编译源文本；成功返回 1，失败返回 0 */
static int compile_source(const char *path, const char *text) {
    g_code_len = 0;
    g_pool_len = 0;
    g_errors = 0;
    g_line_no = 0;
    g_if_depth = 0;
    g_source_path = path;

    char *copy = (char *)malloc(strlen(text) + 1);
    if (!copy) {
        fprintf(stderr, "out of memory\n");
        return 0;
    }
    strcpy(copy, text);

    char *cursor = copy;
    while (*cursor) {
        char *line = cursor;
        while (*cursor && *cursor != '\n') cursor++;
        if (*cursor == '\n') *cursor++ = '\0';

        g_line_no++;
        compile_line(trim_in_place(line));
    }

    free(copy);

    if (g_if_depth != 0) {
        error_at(g_line_no, "missing 'endif' at end of file");
    }

    emit_byte(SLOP_END);

    return g_errors == 0;
}

/* ============================================================
 * 镜像写入 / 读取
 * ============================================================ */

static uint32_t fnv1a(const uint8_t *data, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint32_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int write_image(const char *path) {
    uint8_t image[SLYZ_MAX_FILE];
    memset(image, 0, sizeof(image));

    memcpy(image + 0, SLYZ_MAGIC, 8);
    put_u16(image + 8, SLYZ_VERSION);
    put_u16(image + 10, 0);
    put_u32(image + 12, (uint32_t)g_pool_len);
    put_u32(image + 16, (uint32_t)g_code_len);
    put_u32(image + 20, 0);

    memcpy(image + SLYZ_HEADER_SIZE, g_pool, (size_t)g_pool_len);
    memcpy(image + SLYZ_HEADER_SIZE + g_pool_len, g_code, (size_t)g_code_len);

    uint32_t checksum = fnv1a(image + SLYZ_HEADER_SIZE, g_pool_len + g_code_len);
    put_u32(image + 24, checksum);
    put_u32(image + 28, 0);

    int total = SLYZ_HEADER_SIZE + g_pool_len + g_code_len;
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;

    size_t written = fwrite(image, 1, (size_t)total, fp);
    fclose(fp);
    return written == (size_t)total;
}

/* ============================================================
 * Windows 端字节码执行器（验证编译结果）
 * ============================================================ */

static int vm_last = 0;

static int vm_ask(const char *prompt) {
    printf("%s [Y/N] ", prompt);
    fflush(stdout);

    int c;
    while ((c = getchar()) != EOF) {
        if (c == 'y' || c == 'Y') { printf("y\n"); return 1; }
        if (c == 'n' || c == 'N') { printf("n\n"); return 0; }
    }
    printf("\n");
    return 0;
}

static int vm_run_line(const char *line) {
    printf("[exec] %s\n", line);
    return 1;
}

static int vm_execute(const uint8_t *image, int size, const char *name) {
    if (size < SLYZ_HEADER_SIZE || memcmp(image, SLYZ_MAGIC, 8) != 0) {
        fprintf(stderr, "%s: not a SLYZBIN image\n", name);
        return 0;
    }

    uint16_t version = get_u16(image + 8);
    uint32_t const_size = get_u32(image + 12);
    uint32_t code_size = get_u32(image + 16);
    uint32_t entry = get_u32(image + 20);
    uint32_t checksum = get_u32(image + 24);

    if (version != SLYZ_VERSION) {
        fprintf(stderr, "%s: unsupported version %u\n", name, (unsigned)version);
        return 0;
    }
    if (const_size > SLYZ_MAX_CONST || code_size > SLYZ_MAX_CODE ||
        (uint32_t)size < SLYZ_HEADER_SIZE + const_size + code_size) {
        fprintf(stderr, "%s: invalid sizes\n", name);
        return 0;
    }
    if (entry >= code_size) {
        fprintf(stderr, "%s: bad entry\n", name);
        return 0;
    }

    const uint8_t *pool = image + SLYZ_HEADER_SIZE;
    const uint8_t *code = pool + const_size;

    if (fnv1a(pool, (int)(const_size + code_size)) != checksum) {
        fprintf(stderr, "%s: checksum mismatch\n", name);
        return 0;
    }

    uint32_t pc = entry;
    uint32_t steps = 0;

    while (pc < code_size) {
        if (++steps > SLYZ_MAX_STEPS) {
            fprintf(stderr, "%s: runaway script aborted\n", name);
            return 0;
        }

        uint8_t op = code[pc++];

        switch (op) {
            case SLOP_END:
                return 1;

            case SLOP_NOP:
                break;

            case SLOP_ECHO:
            case SLOP_ASK:
            case SLOP_RUN: {
                if (pc + 2 > code_size) { fprintf(stderr, "%s: bad operand\n", name); return 0; }
                uint16_t off = get_u16(code + pc);
                pc += 2;
                if (off >= const_size) { fprintf(stderr, "%s: bad string offset\n", name); return 0; }
                const char *text = (const char *)(pool + off);
                if (memchr(text, '\0', const_size - off) == NULL) {
                    fprintf(stderr, "%s: unterminated string\n", name);
                    return 0;
                }
                if (op == SLOP_ECHO) printf("%s\n", text);
                else if (op == SLOP_ASK) vm_last = vm_ask(text);
                else if (!vm_run_line(text)) return 0;
                break;
            }

            case SLOP_JMP:
                if (pc + 2 > code_size) { fprintf(stderr, "%s: bad jump\n", name); return 0; }
                pc = get_u16(code + pc);
                break;

            case SLOP_JZ:
                if (pc + 2 > code_size) { fprintf(stderr, "%s: bad jump\n", name); return 0; }
                {
                    uint16_t target = get_u16(code + pc);
                    pc += 2;
                    if (vm_last == 0) pc = target;
                }
                break;

            case SLOP_JNZ:
                if (pc + 2 > code_size) { fprintf(stderr, "%s: bad jump\n", name); return 0; }
                {
                    uint16_t target = get_u16(code + pc);
                    pc += 2;
                    if (vm_last != 0) pc = target;
                }
                break;

            case SLOP_EXIT:
                return 1;

            case SLOP_HELP:
                printf("[help]\n");
                break;

            case SLOP_CLEAR:
                printf("[clear]\n");
                break;

            case SLOP_SET:
                if (pc + 1 > code_size) { fprintf(stderr, "%s: bad set\n", name); return 0; }
                vm_last = code[pc++] ? 1 : 0;
                break;

            default:
                fprintf(stderr, "%s: unknown opcode 0x%02X at %u\n",
                        name, op, (unsigned)(pc - 1));
                return 0;
        }
    }

    return 1;
}

/* ============================================================
 * dump：反汇编
 * ============================================================ */

static int dump_image(const uint8_t *image, int size, const char *name) {
    if (size < SLYZ_HEADER_SIZE || memcmp(image, SLYZ_MAGIC, 8) != 0) {
        fprintf(stderr, "%s: not a SLYZBIN image\n", name);
        return 0;
    }

    uint32_t const_size = get_u32(image + 12);
    uint32_t code_size = get_u32(image + 16);
    if ((uint32_t)size < SLYZ_HEADER_SIZE + const_size + code_size) {
        fprintf(stderr, "%s: truncated\n", name);
        return 0;
    }

    const uint8_t *pool = image + SLYZ_HEADER_SIZE;
    const uint8_t *code = pool + const_size;

    printf("SLYZBIN v%u  const=%u code=%u entry=%u checksum=0x%08X\n",
           (unsigned)get_u16(image + 8), (unsigned)const_size,
           (unsigned)code_size, (unsigned)get_u32(image + 20),
           (unsigned)get_u32(image + 24));

    uint32_t pc = 0;
    while (pc < code_size) {
        uint32_t at = pc;
        uint8_t op = code[pc++];

        switch (op) {
            case SLOP_END:   printf("%04u  END\n", (unsigned)at); break;
            case SLOP_NOP:   printf("%04u  NOP\n", (unsigned)at); break;
            case SLOP_EXIT:  printf("%04u  EXIT\n", (unsigned)at); break;
            case SLOP_HELP:  printf("%04u  HELP\n", (unsigned)at); break;
            case SLOP_CLEAR: printf("%04u  CLEAR\n", (unsigned)at); break;

            case SLOP_ECHO:
            case SLOP_ASK:
            case SLOP_RUN: {
                uint16_t off = get_u16(code + pc);
                pc += 2;
                const char *text = (off < const_size) ? (const char *)(pool + off) : "?";
                const char *mn = (op == SLOP_ECHO) ? "ECHO" : (op == SLOP_ASK) ? "ASK" : "RUN";
                printf("%04u  %-5s \"%s\"\n", (unsigned)at, mn, text);
                break;
            }

            case SLOP_JMP:
            case SLOP_JZ:
            case SLOP_JNZ: {
                uint16_t target = get_u16(code + pc);
                pc += 2;
                const char *mn = (op == SLOP_JMP) ? "JMP" : (op == SLOP_JZ) ? "JZ" : "JNZ";
                printf("%04u  %-5s %04u\n", (unsigned)at, mn, (unsigned)target);
                break;
            }

            case SLOP_SET: {
                uint8_t value = code[pc++];
                printf("%04u  SET   %u\n", (unsigned)at, (unsigned)value);
                break;
            }

            default:
                printf("%04u  DB    0x%02X\n", (unsigned)at, op);
                break;
        }
    }
    return 1;
}

/* ============================================================
 * 文档与用法
 * ============================================================ */

static void print_usage(void) {
    printf(
        "slyzc - SLYZ 二进制编译器\n"
        "\n"
        "用法:\n"
        "  slyztool compile <input.slyz> [-o output.bin]   编译成 SLYZBIN 镜像\n"
        "  slyztool check   <input.slyz>                   只做语法检查\n"
        "  slyztool dump    <input.bin>                    反汇编已编译镜像\n"
        "  slyztool run     <input.slyz>                   编译后在 Windows 上模拟执行\n"
        "  slyztool doc                                    显示语法参考\n"
        "\n"
        "示例:\n"
        "  slyztool compile demo.slyz -o demo.bin\n"
        "  slyztool dump demo.bin\n"
        "\n"
        "把 demo.bin 放进 SlyzFS 后，在系统内执行:\n"
        "  run demo.bin\n"
    );
}

static void show_doc(void) {
    printf(
        "SLYZ 语法参考\n"
        "============================================================\n"
        "echo <text>          输出一行文本\n"
        "ask <prompt>         询问 Y/N，结果写入 last\n"
        "set yes | set no     直接设置 last\n"
        "if yes               last 为真时执行到 endif / else\n"
        "if no                last 为假时执行到 endif / else\n"
        "else                 分支切换\n"
        "endif                结束条件块\n"
        "run <script>         调用另一个脚本\n"
        "clear / cls          清屏\n"
        "help                 显示系统帮助\n"
        "exit / quit          立即结束\n"
        "<其他命令>           整行交给 shell 执行\n"
        "# 开头               注释\n"
        "\n"
        "编译输出:\n"
        "  SLYZBIN v1，32 字节头 + 常量池 + 字节码，全部小端，\n"
        "  无重定位信息，Windows 上编译的镜像可直接在 SaylterOS 执行。\n"
    );
}

/* ============================================================
 * main
 * ============================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "doc") == 0) {
        show_doc();
        return 0;
    }

    if (strcmp(command, "compile") == 0 || strcmp(command, "check") == 0) {
        if (argc < 3) {
            fprintf(stderr, "%s requires an input file\n", command);
            return 1;
        }

        const char *input = argv[2];
        const char *output = NULL;

        for (int i = 3; i < argc; i++) {
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
                output = argv[i + 1];
                i++;
            }
        }

        int size = 0;
        char *text = read_entire_file(input, &size);
        if (!text) {
            fprintf(stderr, "cannot read '%s'\n", input);
            return 1;
        }

        int ok = compile_source(input, text);
        free(text);

        if (!ok) {
            fprintf(stderr, "%s: compilation failed with %d error(s)\n", input, g_errors);
            return 1;
        }

        if (strcmp(command, "check") == 0) {
            printf("%s: OK (const=%d code=%d)\n", input, g_pool_len, g_code_len);
            return 0;
        }

        if (!output) {
            char default_out[1024];
            snprintf(default_out, sizeof(default_out), "%s.bin", input);
            output = default_out;
            if (!write_image(output)) {
                fprintf(stderr, "cannot write '%s'\n", output);
                return 1;
            }
            printf("%s: compiled %d bytes -> %s\n",
                   input, SLYZ_HEADER_SIZE + g_pool_len + g_code_len, output);
            return 0;
        }

        if (!write_image(output)) {
            fprintf(stderr, "cannot write '%s'\n", output);
            return 1;
        }
        printf("%s: compiled %d bytes -> %s\n",
               input, SLYZ_HEADER_SIZE + g_pool_len + g_code_len, output);
        return 0;
    }

    if (strcmp(command, "dump") == 0) {
        if (argc < 3) {
            fprintf(stderr, "dump requires an input file\n");
            return 1;
        }
        int size = 0;
        char *data = read_entire_file(argv[2], &size);
        if (!data) {
            fprintf(stderr, "cannot read '%s'\n", argv[2]);
            return 1;
        }
        int ok = dump_image((const uint8_t *)data, size, argv[2]);
        free(data);
        return ok ? 0 : 1;
    }

    if (strcmp(command, "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "run requires an input .slyz\n");
            return 1;
        }

        int size = 0;
        char *text = read_entire_file(argv[2], &size);
        if (!text) {
            fprintf(stderr, "cannot read '%s'\n", argv[2]);
            return 1;
        }

        int ok = compile_source(argv[2], text);
        free(text);

        if (!ok) {
            fprintf(stderr, "%s: compilation failed\n", argv[2]);
            return 1;
        }

        uint8_t image[SLYZ_MAX_FILE];
        memset(image, 0, sizeof(image));
        memcpy(image, SLYZ_MAGIC, 8);
        put_u16(image + 8, SLYZ_VERSION);
        put_u32(image + 12, (uint32_t)g_pool_len);
        put_u32(image + 16, (uint32_t)g_code_len);
        put_u32(image + 20, 0);
        memcpy(image + SLYZ_HEADER_SIZE, g_pool, (size_t)g_pool_len);
        memcpy(image + SLYZ_HEADER_SIZE + g_pool_len, g_code, (size_t)g_code_len);
        put_u32(image + 24, fnv1a(image + SLYZ_HEADER_SIZE, g_pool_len + g_code_len));

        int total = SLYZ_HEADER_SIZE + g_pool_len + g_code_len;
        return vm_execute(image, total, argv[2]) ? 0 : 1;
    }

    fprintf(stderr, "unknown command '%s'\n\n", command);
    print_usage();
    return 1;
}
