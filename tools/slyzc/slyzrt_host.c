/* ============================================================
 * slyzrt_host.c - Windows 端 SLYZ 运行时
 * ------------------------------------------------------------
 * 让 `slyzc run` 能在 Windows 上直接验证编译结果。它只提供
 * SlyzVmOps 的宿主侧实现：内存、日志、系统调用。指令语义仍然
 * 由 src/kernel/slyzvm.c 提供，与裸机系统完全同一份代码。
 *
 * 边界：窗口/绘图/事件调用在命令行环境下没有承载窗口，统一
 * 返回 -1，并在第一次遇到时打印一次说明。GUI 应用的真正验证
 * 要在 SaylterOS 桌面里完成。
 * ============================================================ */

#include "slyzvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ---------- 基础回调 ---------- */

static void *host_alloc(uint32_t n) {
    return malloc(n ? n : 1);
}

static void host_free(void *p) {
    free(p);
}

static void host_tick(void) {
}

static int g_logged_once[16];

/* 命令行宿主上的 SylFault 配置影子：不写文件，但读写语义与内核一致，
 * 让同一份 .slyzcode 在两个宿主上都能完整跑完界面流程。 */
static int g_host_dump = 0;
static int g_host_serial = 0;

static void host_note_once(int slot, const char *text) {
    if (slot < 0 || slot >= 16) return;
    if (g_logged_once[slot]) return;
    g_logged_once[slot] = 1;
    fprintf(stderr, "slyzc: %s\n", text);
}

static void host_log(const char *text) {
    if (text) fprintf(stderr, "slyz: %s\n", text);
}

/* ---------- 参数读取 ---------- */

static const char *arg_text(struct SlyzVal *args, uint16_t argc, int index, int *len) {
    if (index < 0 || index >= (int)argc) {
        if (len) *len = 0;
        return "";
    }
    const char *s = slyz_val_str(&args[index], len);
    if (!s) {
        if (len) *len = 0;
        return "";
    }
    return s;
}

static int64_t arg_int(struct SlyzVal *args, uint16_t argc, int index) {
    if (index < 0 || index >= (int)argc) return 0;
    if (args[index].type == SLYZ_T_INT) return args[index].u.i;
    if (args[index].type == SLYZ_T_STR) {
        int len = 0;
        const char *s = slyz_val_str(&args[index], &len);
        if (!s) return 0;
        int neg = 0;
        int i = 0;
        int64_t v = 0;
        if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); i++; }
        while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
        return neg ? -v : v;
    }
    if (args[index].type == SLYZ_T_ARR) {
        return (int64_t)slyz_val_arr_len(&args[index]);
    }
    return 0;
}

/* ---------- 文本输出 ---------- */

static void print_value(struct SlyzVal *v, int newline) {
    char buf[512];
    int n = slyz_val_to_text(0, v, buf, (int)sizeof(buf));
    if (n > 0) fwrite(buf, 1, (size_t)n, stdout);
    if (newline) fputc('\n', stdout);
    fflush(stdout);
}

/* ---------- 文件 ---------- */

static char *read_file_all(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    if (n < 0 || n > (long)(SLYZ_MAX_CONST * 4)) { fclose(f); return 0; }
    rewind(f);

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }

    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = (int)got;
    return buf;
}

static int write_file_all(const char *path, const char *data, int len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t put = fwrite(data, 1, (size_t)len, f);
    int rc = (fclose(f) == 0 && put == (size_t)len) ? len : -1;
    return rc;
}

/* ---------- 配置 ---------- */

static char *cfg_path(void) {
    const char *candidates[] = {
        "etc/system.cfg",
        "../etc/system.cfg",
        0
    };

    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return (char *)candidates[i];
        }
    }
    return (char *)candidates[0];
}

static int cfg_get_value(const char *key, char *out, int cap) {
    out[0] = 0;

    int len = 0;
    char *data = read_file_all(cfg_path(), &len);
    if (!data) return 0;

    int key_len = (int)strlen(key);
    int i = 0;
    int found = 0;

    while (i < len && !found) {
        int start = i;
        while (i < len && data[i] != '\n') i++;
        int end = i;
        if (i < len) i++;

        if (end > start && data[start] == '#') continue;

        int eq = -1;
        for (int k = start; k < end; k++) {
            if (data[k] == '=') { eq = k; break; }
        }
        if (eq < 0) continue;
        if (eq - start != key_len) continue;
        if (memcmp(data + start, key, (size_t)key_len) != 0) continue;

        int v = eq + 1;
        while (v < end && (data[v] == ' ' || data[v] == '\t')) v++;
        while (end > v && (data[end - 1] == '\r' || data[end - 1] == ' ' ||
                           data[end - 1] == '\t')) end--;

        int n = end - v;
        if (n > cap - 1) n = cap - 1;
        memcpy(out, data + v, (size_t)n);
        out[n] = 0;
        found = 1;
    }

    free(data);
    return found;
}

static int cfg_set_value(const char *key, const char *value) {
    int len = 0;
    char *data = read_file_all(cfg_path(), &len);
    if (!data) {
        data = (char *)malloc(1);
        data[0] = 0;
        len = 0;
    }

    int key_len = (int)strlen(key);
    int value_len = (int)strlen(value);

    char *out = (char *)malloc((size_t)len + (size_t)value_len + (size_t)key_len + 64);
    int o = 0;
    int replaced = 0;

    int i = 0;
    while (i < len) {
        int start = i;
        while (i < len && data[i] != '\n') i++;
        int end = i;
        int had_newline = 0;
        if (i < len) { i++; had_newline = 1; }

        int eq = -1;
        for (int k = start; k < end; k++) {
            if (data[k] == '=') { eq = k; break; }
        }

        if (eq >= 0 && eq - start == key_len &&
            memcmp(data + start, key, (size_t)key_len) == 0 && !replaced) {
            memcpy(out + o, key, (size_t)key_len);
            o += key_len;
            out[o++] = '=';
            memcpy(out + o, value, (size_t)value_len);
            o += value_len;
            if (had_newline) out[o++] = '\n';
            replaced = 1;
            continue;
        }

        memcpy(out + o, data + start, (size_t)(end - start));
        o += end - start;
        if (had_newline) out[o++] = '\n';
    }

    if (!replaced) {
        memcpy(out + o, key, (size_t)key_len);
        o += key_len;
        out[o++] = '=';
        memcpy(out + o, value, (size_t)value_len);
        o += value_len;
        out[o++] = '\n';
    }

    int rc = write_file_all(cfg_path(), out, o);
    free(out);
    free(data);
    return rc >= 0 ? 0 : -1;
}

/* ---------- 系统调用 ---------- */

static int host_sys(struct SlyzVm *vm, uint16_t id,
                    struct SlyzVal *args, uint16_t argc,
                    struct SlyzVal *ret) {
    switch (id) {
        case SLE_SYS_PRINT:
            if (argc >= 1) print_value(&args[0], 0);
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_PRINTLN:
            if (argc >= 1) print_value(&args[0], 1);
            else fputc('\n', stdout);
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_EPRINT:
            if (argc >= 1) {
                char buf[512];
                int n = slyz_val_to_text(vm, &args[0], buf, (int)sizeof(buf));
                if (n > 0) fwrite(buf, 1, (size_t)n, stderr);
            }
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_FS_READ: {
            const char *path = arg_text(args, argc, 0, 0);
            int len = 0;
            char *data = read_file_all(path, &len);
            if (!data) { slyz_ret_nil(ret); return 0; }
            if (!slyz_ret_str(vm, ret, data, len)) {
                free(data);
                return -1;
            }
            free(data);
            return 0;
        }

        case SLE_SYS_FS_WRITE: {
            const char *path = arg_text(args, argc, 0, 0);
            int len = 0;
            const char *text = arg_text(args, argc, 1, &len);
            slyz_ret_int(ret, (int64_t)write_file_all(path, text, len));
            return 0;
        }

        case SLE_SYS_FS_EXISTS: {
            const char *path = arg_text(args, argc, 0, 0);
            FILE *f = fopen(path, "rb");
            if (f) fclose(f);
            slyz_ret_int(ret, f ? 1 : 0);
            return 0;
        }

        case SLE_SYS_FS_REMOVE: {
            const char *path = arg_text(args, argc, 0, 0);
            slyz_ret_int(ret, remove(path) == 0 ? 0 : -1);
            return 0;
        }

        case SLE_SYS_FS_MKDIR: {
            const char *path = arg_text(args, argc, 0, 0);
#ifdef _WIN32
            int rc = _mkdir(path);
#else
            int rc = mkdir(path, 0755);
#endif
            slyz_ret_int(ret, rc == 0 ? 0 : -1);
            return 0;
        }

        case SLE_SYS_FS_LIST: {
            /* 命令行宿主只验证语法与流程，目录枚举在 GUI/内核环境下才完整实现 */
            host_note_once(0, "fs.list is not available in the host runner");
            if (!slyz_ret_arr(vm, ret, 0)) return -1;
            return 0;
        }

        case SLE_SYS_CFG_GET: {
            const char *key = arg_text(args, argc, 0, 0);
            char buf[256];
            if (!cfg_get_value(key, buf, (int)sizeof(buf))) {
                slyz_ret_str(vm, ret, "", 0);
            } else {
                if (!slyz_ret_str(vm, ret, buf, (int)strlen(buf))) return -1;
            }
            return 0;
        }

        case SLE_SYS_CFG_SET: {
            const char *key = arg_text(args, argc, 0, 0);
            const char *value = arg_text(args, argc, 1, 0);
            slyz_ret_int(ret, (int64_t)cfg_set_value(key, value));
            return 0;
        }

        case SLE_SYS_CFG_KEYS: {
            if (!slyz_ret_arr(vm, ret, 0)) return -1;
            return 0;
        }

        case SLE_SYS_TIME_NOW: {
            slyz_ret_int(ret, (int64_t)time(0));
            return 0;
        }

        case SLE_SYS_TIME_MS: {
            slyz_ret_int(ret, (int64_t)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC));
            return 0;
        }

        case SLE_SYS_TIME_TEXT: {
            time_t now = time(0);
            struct tm *tmv = localtime(&now);
            char buf[16];
            if (tmv) {
                snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                         tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
            } else {
                snprintf(buf, sizeof(buf), "00:00:00");
            }
            if (!slyz_ret_str(vm, ret, buf, (int)strlen(buf))) return -1;
            return 0;
        }

        case SLE_SYS_DATE_TEXT: {
            time_t now = time(0);
            struct tm *tmv = localtime(&now);
            char buf[40];
            if (tmv) {
                snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                         tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday);
            } else {
                snprintf(buf, sizeof(buf), "1970-01-01");
            }
            if (!slyz_ret_str(vm, ret, buf, (int)strlen(buf))) return -1;
            return 0;
        }

        case SLE_SYS_SLEEP: {
            int64_t ms = arg_int(args, argc, 0);
            if (ms < 0) ms = 0;
#ifdef _WIN32
            Sleep((DWORD)ms);
#else
            usleep((useconds_t)(ms * 1000));
#endif
            slyz_ret_nil(ret);
            return 0;
        }

        case SLE_SYS_BEEP:
            host_note_once(1, "os.beep is not available in the host runner");
            slyz_ret_nil(ret);
            return 0;

        case SLE_SYS_EXEC: {
            const char *cmd = arg_text(args, argc, 0, 0);
            int rc = system(cmd);
            slyz_ret_int(ret, (int64_t)rc);
            return 0;
        }

        case SLE_SYS_EXIT: {
            int64_t code = arg_int(args, argc, 0);
            slyzvm_request_exit(vm, (int)code);
            slyz_ret_nil(ret);
            return 0;
        }

        /* 故障注入在命令行宿主上不执行：开发机不能被测试程序打崩。
         * 配置读写仍然生效，这样 SylFault 的界面流程可以完整走通。 */
        case SLE_SYS_FAULT_TRIGGER:
            host_note_once(3, "fault.trigger is only available inside SaylterOS");
            slyz_ret_int(ret, -1);
            return 0;

        case SLE_SYS_FAULT_DUMP:
            host_note_once(4, "fault.dump needs the SaylterOS kernel heap and SlyzFS");
            slyz_ret_int(ret, -1);
            return 0;

        case SLE_SYS_FAULT_LEAK:
            host_note_once(5, "fault.leak only leaks inside the SaylterOS kernel");
            slyz_ret_int(ret, 0);
            return 0;

        case SLE_SYS_FAULT_LEAK_FREE:
            slyz_ret_int(ret, 0);
            return 0;

        case SLE_SYS_FAULT_SET: {
            const char *key = arg_text(args, argc, 0, 0);
            const char *value = arg_text(args, argc, 1, 0);
            int flag = (value && (value[0] == '1' || value[0] == 'o' ||
                                  value[0] == 't' || value[0] == 'y')) ? 1 : 0;
            if (strcmp(key, "dump") == 0) {
                g_host_dump = flag;
                slyz_ret_int(ret, 0);
                return 0;
            }
            if (strcmp(key, "serial_to_disk") == 0) {
                g_host_serial = flag;
                slyz_ret_int(ret, 0);
                return 0;
            }
            slyz_ret_int(ret, -1);
            return 0;
        }

        case SLE_SYS_FAULT_GET: {
            const char *key = arg_text(args, argc, 0, 0);
            const char *value = 0;
            if (strcmp(key, "dump") == 0) value = g_host_dump ? "1" : "0";
            else if (strcmp(key, "serial_to_disk") == 0) value = g_host_serial ? "1" : "0";
            else if (strcmp(key, "leak_kb") == 0) value = "0";
            if (!value) {
                slyz_ret_int(ret, -1);
                return 0;
            }
            if (!slyz_ret_str(vm, ret, value, (int)strlen(value))) return -1;
            return 0;
        }

        case SLE_SYS_INPUT: {
            const char *prompt = arg_text(args, argc, 0, 0);
            char line[256];
            if (prompt && *prompt) {
                fputs(prompt, stdout);
                fflush(stdout);
            }
            if (!fgets(line, (int)sizeof(line), stdin)) {
                line[0] = '\0';
            }
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (!slyz_ret_str(vm, ret, line, (int)len)) return -1;
            return 0;
        }

        case SLE_SYS_WIN_OPEN:
        case SLE_SYS_WIN_CLOSE:
        case SLE_SYS_WIN_SIZE:
        case SLE_SYS_WIN_HEIGHT:
        case SLE_SYS_WIN_TITLE:
        case SLE_SYS_GFX_CLEAR:
        case SLE_SYS_GFX_RECT:
        case SLE_SYS_GFX_FRAME:
        case SLE_SYS_GFX_TEXT:
        case SLE_SYS_GFX_TWIDTH:
        case SLE_SYS_GFX_LINE:
        case SLE_SYS_GFX_CIRCLE:
        case SLE_SYS_GFX_FLUSH:
        case SLE_SYS_EVT_POLL:
        case SLE_SYS_EVT_WAIT:
            host_note_once(2, "window/graphics calls need the SaylterOS graphical host");
            slyz_ret_int(ret, -1);
            return 0;

        default:
            slyz_ret_int(ret, -1);
            return 0;
    }
}

static const struct SlyzVmOps g_host_ops = {
    host_sys,
    host_alloc,
    host_free,
    host_tick,
    host_log
};

int slyz_host_run(const uint8_t *image, uint32_t size) {
    struct SlyzVm *vm = slyzvm_create(image, (int)size, &g_host_ops, "host");
    if (!vm) {
        fprintf(stderr, "slyzc: cannot load image (bad format, version or checksum)\n");
        return 1;
    }

    for (;;) {
        int st = slyzvm_step(vm, 65536);
        if (st == 0) break;
        if (st < 0) {
            fprintf(stderr, "slyzc: runtime error: %s (pc=%u, steps=%u)\n",
                    slyzvm_error(vm), 0u, (unsigned)slyzvm_steps(vm));
            slyzvm_destroy(vm);
            return 2;
        }
    }

    int code = slyzvm_exit_code(vm);
    slyzvm_destroy(vm);
    return code;
}
