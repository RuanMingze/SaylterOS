/* ============================================================
 * slyzc_main.c - 命令行入口
 * ------------------------------------------------------------
 * 用法：
 *   slyzc compile <源码.slyzcode> [-o 产物.slyz]
 *   slyzc check   <源码.slyzcode>
 *   slyzc dump    <产物.slyz>
 *   slyzc run     <产物.slyz>
 *   slyzc doc
 *
 * 源码扩展名固定为 .slyzcode，产物扩展名固定为 .slyz。
 * compile 与 check 共用同一条解析/生成流水线，check 只是不写文件，
 * 因此"能通过 check 的源码一定能产生可执行镜像"这一点由构建流程
 * 保证，而不是靠两套规则各写一遍。
 * ============================================================ */

#include "slyzc.h"
#include <setjmp.h>
#include <stdarg.h>

Compiler  g_cc;
extern jmp_buf g_parse_jmp;

extern int slyz_host_run(const uint8_t *image, uint32_t size);

/* ---------- 文件读写 ---------- */

static uint8_t *read_whole_file(const char *path, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "slyzc: cannot open '%s'\n", path);
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "slyzc: cannot seek '%s'\n", path);
        return 0;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        fprintf(stderr, "slyzc: cannot measure '%s'\n", path);
        return 0;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)xmalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);

    if (got != (size_t)n) {
        free(buf);
        fprintf(stderr, "slyzc: short read on '%s'\n", path);
        return 0;
    }
    buf[n] = 0;
    *out_size = (uint32_t)n;
    return buf;
}

static int write_whole_file(const char *path, const uint8_t *data, uint32_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "slyzc: cannot create '%s'\n", path);
        return 0;
    }
    size_t put = fwrite(data, 1, (size_t)size, f);
    if (fclose(f) != 0 || put != (size_t)size) {
        fprintf(stderr, "slyzc: write failed for '%s'\n", path);
        return 0;
    }
    return 1;
}

/* ---------- 路径工具 ---------- */

static int has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static char *make_output_name(const char *source) {
    size_t n = strlen(source);
    size_t cut = n;
    const char *dot = strrchr(source, '.');
    if (dot && has_suffix(source, ".slyzcode")) cut = (size_t)(dot - source);

    char *out = (char *)xmalloc(cut + 6);
    memcpy(out, source, cut);
    memcpy(out + cut, ".slyz", 6);
    return out;
}

/* ---------- 编译 ---------- */

static int cmd_compile(const char *source_path, const char *out_path,
                       int write_file, int json) {
    uint32_t size = 0;
    uint8_t *src = read_whole_file(source_path, &size);
    if (!src) {
        if (json) {
            printf("{\"ok\":false,\"errorCount\":1,\"diagnostics\":"
                   "[{\"line\":0,\"col\":0,\"severity\":\"error\",\"message\":");
            slyz_write_json_string(stdout, "cannot open source file");
            printf("}]}\n");
        }
        return 1;
    }

    if (!has_suffix(source_path, ".slyzcode")) {
        if (!json) {
            fprintf(stderr, "slyzc: warning: source file should use the .slyzcode extension\n");
        }
    }

    if (json) slyz_diag_set_quiet(1);

    int ok = slyz_compile_source(source_path, src, size);
    uint8_t *image = 0;
    uint32_t image_size = 0;
    int wrote = 0;

    if (ok && write_file) {
        if (!compile_image(&g_cc, &image, &image_size)) {
            ok = 0;
        } else {
            char *generated = 0;
            const char *target = out_path;
            if (!target) {
                generated = make_output_name(source_path);
                target = generated;
            }
            if (!write_whole_file(target, image, image_size)) {
                ok = 0;
            } else {
                wrote = 1;
            }
            if (!json && wrote) {
                printf("slyzc: wrote %s (%u bytes, code %u, const %u, globals %d, functions %d)\n",
                       target, (unsigned)image_size,
                       (unsigned)g_cc.code_len, (unsigned)g_cc.consts.len,
                       g_cc.global_count, g_cc.func_count);
            }
            free(generated);
        }
    }

    if (json) {
        printf("{\"ok\":%s,\"errorCount\":%d,\"diagnostics\":",
               ok ? "true" : "false", g_cc.error_count);
        slyz_write_diagnostics_json(stdout);
        if (ok) {
            fputs(",\"stats\":", stdout);
            slyz_write_stats_json(stdout);
        }
        fputs("}\n", stdout);
        fflush(stdout);
    } else if (!write_file) {
        if (ok) {
            printf("slyzc: OK  code=%u const=%u globals=%d functions=%d stack=%d%s\n",
                   (unsigned)g_cc.code_len, (unsigned)g_cc.consts.len,
                   g_cc.global_count, g_cc.func_count, g_cc.max_stack + 32,
                   g_cc.uses_gui ? " gui" : "");
        } else {
            fprintf(stderr, "slyzc: %d error(s)\n", g_cc.error_count);
        }
    } else if (!ok) {
        fprintf(stderr, "slyzc: %d error(s), no image written\n", g_cc.error_count);
    }

    free(image);
    free(src);
    return ok ? 0 : 1;
}

/* ---------- 反汇编 ---------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return (int64_t)v;
}

static const char *op_name(uint8_t op) {
    switch (op) {
        case SLE_OP_HALT: return "HALT";
        case SLE_OP_NOP: return "NOP";
        case SLE_OP_POP: return "POP";
        case SLE_OP_DUP: return "DUP";
        case SLE_OP_SWAP: return "SWAP";
        case SLE_OP_ADD: return "ADD";
        case SLE_OP_SUB: return "SUB";
        case SLE_OP_MUL: return "MUL";
        case SLE_OP_DIV: return "DIV";
        case SLE_OP_MOD: return "MOD";
        case SLE_OP_NEG: return "NEG";
        case SLE_OP_EQ: return "EQ";
        case SLE_OP_NE: return "NE";
        case SLE_OP_LT: return "LT";
        case SLE_OP_LE: return "LE";
        case SLE_OP_GT: return "GT";
        case SLE_OP_GE: return "GE";
        case SLE_OP_AND: return "AND";
        case SLE_OP_OR: return "OR";
        case SLE_OP_NOT: return "NOT";
        case SLE_OP_RET: return "RET";
        case SLE_OP_PUSH_NIL: return "PUSH_NIL";
        case SLE_OP_PUSH_TRUE: return "PUSH_TRUE";
        case SLE_OP_PUSH_FALSE: return "PUSH_FALSE";
        case SLE_OP_IDX_GET: return "IDX_GET";
        case SLE_OP_IDX_SET: return "IDX_SET";
        case SLE_OP_TYPEOF: return "TYPEOF";
        case SLE_OP_DUP2: return "DUP2";
        case SLE_OP_PUSH_I: return "PUSH_I";
        case SLE_OP_PUSH_S: return "PUSH_S";
        case SLE_OP_LOAD_L: return "LOAD_L";
        case SLE_OP_STORE_L: return "STORE_L";
        case SLE_OP_LOAD_G: return "LOAD_G";
        case SLE_OP_STORE_G: return "STORE_G";
        case SLE_OP_JMP: return "JMP";
        case SLE_OP_JZ: return "JZ";
        case SLE_OP_JNZ: return "JNZ";
        case SLE_OP_CALL: return "CALL";
        case SLE_OP_MAKE_ARR: return "MAKE_ARR";
        case SLE_OP_SYS: return "SYS";
        default: return "???";
    }
}

static void dump_const(const uint8_t *pool, uint32_t pool_size, uint32_t off) {
    if (off >= pool_size) {
        printf("\"<bad offset %u>\"", off);
        return;
    }
    const char *s = (const char *)(pool + off);
    uint32_t left = pool_size - off;
    uint32_t n = 0;
    while (n < left && s[n]) n++;
    if (n == left) {
        printf("\"<unterminated>\"");
        return;
    }

    printf("\"");
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c == '\r') printf("\\r");
        else if (c == '"') printf("\\\"");
        else if (c == '\\') printf("\\\\");
        else if (c >= 32 && c < 127) putchar(c);
        else printf("\\x%02X", c);
    }
    printf("\"");
}

static int cmd_dump(const char *path) {
    uint32_t size = 0;
    uint8_t *img = read_whole_file(path, &size);
    if (!img) return 1;

    if (size < SLYZ_HEADER_SIZE || memcmp(img, "SLYZBIN", 7) != 0) {
        fprintf(stderr, "slyzc: '%s' is not a SLYZBIN image\n", path);
        free(img);
        return 1;
    }

    uint16_t version = rd16(img + SLYZ_HDR_VERSION);
    uint16_t flags = rd16(img + SLYZ_HDR_FLAGS);
    uint32_t const_size = rd32(img + SLYZ_HDR_CONSTSIZE);
    uint32_t code_size = rd32(img + SLYZ_HDR_CODESIZE);
    uint32_t entry = rd32(img + SLYZ_HDR_ENTRY);
    uint32_t globals = rd32(img + SLYZ_HDR_GLOBALS);
    uint32_t stack_hint = rd32(img + SLYZ_HDR_STACKHINT);

    printf("SLYZBIN v%u  flags=0x%04X  const=%u  code=%u  entry=%u  globals=%u  stack=%u\n",
           (unsigned)version, (unsigned)flags,
           (unsigned)const_size, (unsigned)code_size, (unsigned)entry,
           (unsigned)globals, (unsigned)stack_hint);

    if (version != SLYZ_VERSION) {
        fprintf(stderr, "slyzc: unsupported version %u (expected %u)\n",
                (unsigned)version, (unsigned)SLYZ_VERSION);
        free(img);
        return 1;
    }

    if ((uint64_t)SLYZ_HEADER_SIZE + const_size + code_size > size) {
        fprintf(stderr, "slyzc: image is truncated\n");
        free(img);
        return 1;
    }

    const uint8_t *pool = img + SLYZ_HEADER_SIZE;
    const uint8_t *code = pool + const_size;

    printf("\n--- constant pool ---\n");
    uint32_t off = 0;
    int idx = 0;
    while (off < const_size) {
        printf("  [%d] @%u ", idx, off);
        dump_const(pool, const_size, off);
        printf("\n");
        uint32_t n = 0;
        while (off + n < const_size && pool[off + n]) n++;
        off += n + 1;
        idx++;
    }

    printf("\n--- code ---\n");
    uint32_t pc = 0;
    while (pc < code_size) {
        uint32_t at = pc;
        uint8_t op = code[pc++];
        printf("  %04X  %-10s", at, op_name(op));

        switch (op) {
            case SLE_OP_PUSH_I:
                if (pc + 8 > code_size) { printf(" <truncated>\n"); pc = code_size; break; }
                printf(" %lld", (long long)rd64(code + pc));
                pc += 8;
                break;

            case SLE_OP_PUSH_S:
                if (pc + 4 > code_size) { printf(" <truncated>\n"); pc = code_size; break; }
                dump_const(pool, const_size, rd32(code + pc));
                pc += 4;
                break;

            case SLE_OP_LOAD_L:
            case SLE_OP_STORE_L:
            case SLE_OP_LOAD_G:
            case SLE_OP_STORE_G:
            case SLE_OP_JMP:
            case SLE_OP_JZ:
            case SLE_OP_JNZ:
            case SLE_OP_MAKE_ARR:
                if (pc + 4 > code_size) { printf(" <truncated>\n"); pc = code_size; break; }
                printf(" %u", rd32(code + pc));
                pc += 4;
                break;

            case SLE_OP_CALL:
                if (pc + 8 > code_size) { printf(" <truncated>\n"); pc = code_size; break; }
                printf(" %u argc=%u", rd32(code + pc), rd32(code + pc + 4));
                pc += 8;
                break;

            case SLE_OP_SYS:
                if (pc + 4 > code_size) { printf(" <truncated>\n"); pc = code_size; break; }
                printf(" id=%u argc=%u", rd16(code + pc), rd16(code + pc + 2));
                pc += 4;
                break;

            default:
                break;
        }

        printf("\n");
    }

    free(img);
    return 0;
}

/* ---------- 文档 ---------- */

static void print_doc(void) {
    printf(
        "SLYZ v2 语言速览\n"
        "================\n"
        "源码扩展名 .slyzcode，编译产物 .slyz（SLYZBIN v2 字节码）。\n"
        "\n"
        "变量与类型\n"
        "  let x = 1            整数（有符号 64 位）\n"
        "  let s = \"text\"       字符串（UTF-8）\n"
        "  let a = [1, 2, 3]     数组\n"
        "  let n = nil           空值\n"
        "  x += 1               复合赋值，另有 -= *= /= %%=\n"
        "\n"
        "表达式\n"
        "  + - * / %%            算术；字符串用 + 拼接\n"
        "  == != < <= > >=       比较\n"
        "  && || !  or and not   逻辑\n"
        "  a[i]  a.at(i)  arr.length  索引与长度\n"
        "  f(x)  obj.push(x)     函数调用与方法调用\n"
        "\n"
        "控制流\n"
        "  if c { } else if d { } else { }\n"
        "  while c { }\n"
        "  for i in 0..10 { }\n"
        "  break / continue / return\n"
        "\n"
        "函数\n"
        "  fn add(a, b) { return a + b }\n"
        "  fn main() { println(add(1, 2)) }\n"
        "  存在 main() 时，顶层语句先执行，再调用 main。\n"
        "\n"
        "内置函数\n"
        "  print println eprint\n"
        "  len str int substr index_of upper lower trim replace split join\n"
        "  abs min max clamp sign random array push pop range typeof\n"
        "  str_at ord chr hex bin\n"
        "\n"
        "文件与配置\n"
        "  fs.read(path) fs.write(path, text) fs.exists(path)\n"
        "  fs.list(path) fs.remove(path) fs.mkdir(path)\n"
        "  cfg.get(key) cfg.set(key, value) cfg.keys()\n"
        "\n"
        "时间与进程\n"
        "  time.now() time.ms() time.text() time.date() time.sleep(ms)\n"
        "  os.exec(cmdline) os.exit(code) os.beep(freq, ms)\n"
        "\n"
        "窗口与绘图（需要 GUI 宿主）\n"
        "  let w = win.open(\"Title\", 640, 480)\n"
        "  gfx.clear(color) gfx.rect(x,y,w,h,color)\n"
        "  gfx.frame(x,y,w,h,color,thickness)\n"
        "  gfx.text(x,y,text,color) gfx.text_width(text)\n"
        "  gfx.line(x1,y1,x2,y2,color) gfx.circle(cx,cy,r,color)\n"
        "  gfx.flush()\n"
        "  let ev = event.poll()   event.wait(timeout_ms)\n"
        "  事件数组：[type, a, b, c]\n"
        "    1 键盘   [1, 字符码, 扫描码, 0]\n"
        "    2 鼠标   [2, x, y, buttons]\n"
        "    3 点击   [3, x, y, button]\n"
        "    4 滚轮   [4, delta, 0, 0]\n"
        "    5 关闭   [5, 0, 0, 0]\n"
        "    7 尺寸   [7, width, height, 0]\n"
        "\n"
        "编译与运行\n"
        "  slyzc compile app.slyzcode -o app.slyz\n"
        "  slyzc check   app.slyzcode\n"
        "  slyzc check   app.slyzcode --json   输出一行 JSON 诊断\n"
        "  slyzc serve                         常驻检查服务（编辑器插件用）\n"
        "  slyzc run app.slyz\n"
        "  slyzc dump app.slyz\n"
        "\n"
        "常驻协议（stdin/stdout，每行一个 JSON）\n"
        "  请求 {\"id\":1,\"path\":\"a.slyzcode\",\"text\":\"...\"}\n"
        "  响应 {\"id\":1,\"ok\":true,\"errorCount\":0,\"diagnostics\":[],\"stats\":{...}}\n"
        "  结束 {\"method\":\"exit\"}\n"
    );
}

/* ---------- 入口 ---------- */

static void usage(void) {
    fprintf(stderr,
            "slyzc - SLYZ v2 compiler\n"
            "usage:\n"
            "  slyzc compile <source.slyzcode> [-o out.slyz] [--json]\n"
            "  slyzc check   <source.slyzcode> [--json]\n"
            "  slyzc dump    <image.slyz>\n"
            "  slyzc run     <image.slyz>\n"
            "  slyzc serve            常驻检查服务（JSON Lines，stdin/stdout）\n"
            "  slyzc doc\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "doc") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_doc();
        return 0;
    }

    if (strcmp(cmd, "serve") == 0) {
        return slyz_serve_main();
    }

    if (strcmp(cmd, "compile") == 0 || strcmp(cmd, "check") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        const char *out = 0;
        int json = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--json") == 0) {
                json = 1;
            }
        }
        return cmd_compile(argv[2], out, strcmp(cmd, "compile") == 0, json);
    }

    if (strcmp(cmd, "dump") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_dump(argv[2]);
    }

    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) { usage(); return 1; }
        uint32_t size = 0;
        uint8_t *img = read_whole_file(argv[2], &size);
        if (!img) return 1;
        int rc = slyz_host_run(img, size);
        free(img);
        return rc;
    }

    fprintf(stderr, "slyzc: unknown command '%s'\n", cmd);
    usage();
    return 1;
}
