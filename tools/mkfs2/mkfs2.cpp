/*
 * mkfs2.cpp - 用 SlyzFS v2 模块格式化镜像并预置文件
 * ------------------------------------------------------------
 * 旧 mkfs 直接链接 C++ SlyzFS 实现；v2 的文件系统本体是
 * build/slyzfs2.bin（slyz3c --baremetal + nasm 的产物）。
 * 本工具把模块加载到固定地址，用文件镜像充当"磁盘"，然后：
 *   1. SFS_OP_FORMAT_DISK 建立分区表并格式化 C:/R:/D:；
 *   2. SFS_OP_MOUNT_ALL 挂载；
 *   3. 逐个 <src>=<dest> 调 OPEN/WRITE/CLOSE 写入。
 *
 * 用法：
 *   mkfs2 <image.img> <module.bin> <src>=<dest> [...]
 * dest 必须以 C:\ / R:\ / D:\ 开头（或省略盘符，默认 C:）。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

#include "slysfs.h"

typedef int64_t (*FsCall)(int64_t op, int64_t a, int64_t b, int64_t c, int64_t d);

static FILE *g_img;
static uint32_t g_sectors;

static int img_read(uint32_t lba, void *buf) {
    if (!g_img || lba >= g_sectors) return 0;
    if (_fseeki64(g_img, (long long)lba * 512LL, SEEK_SET) != 0) return 0;
    return fread(buf, 1, 512, g_img) == 512 ? 1 : 0;
}

static int img_write(uint32_t lba, const void *buf) {
    if (!g_img || lba >= g_sectors) return 0;
    if (_fseeki64(g_img, (long long)lba * 512LL, SEEK_SET) != 0) return 0;
    return fwrite(buf, 1, 512, g_img) == 512 ? 1 : 0;
}

static uint32_t img_sectors(void) { return g_sectors; }
static void img_log(const char *s) { std::fputs(s, stderr); }
static int img_flush(void) {
    if (g_img) std::fflush(g_img);
    return 1;
}

static unsigned char *read_file(const char *path, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "mkfs2: cannot open '%s'\n", path);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        std::fprintf(stderr, "mkfs2: '%s' is empty\n", path);
        return nullptr;
    }
    unsigned char *buf = (unsigned char *)std::malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        std::free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    *out_size = (uint32_t)n;
    return buf;
}

static void drive_and_path(const char *dest, int *drive, const char **rel) {
    int d = 0;
    const char *p = dest;
    if (p[0] && p[1] == ':') {
        char c = (char)(p[0] | 0x20);
        if (c == 'r') d = 1;
        else if (c == 'd') d = 2;
        p += 2;
        if (*p == '\\' || *p == '/') p++;
    }
    *drive = d;
    *rel = p;
}

/* Windows 的 argv 是 ANSI（简体中文机器为 GBK），v2 统一按 UTF-8
 * 存文件名。只转换目标路径；源路径交给 fopen 按 ANSI 解析。 */
static void dest_to_utf8(const char *src, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
#ifdef _WIN32
    wchar_t wide[1024];
    int wn = MultiByteToWideChar(CP_ACP, 0, src, -1, wide,
                                 (int)(sizeof(wide) / sizeof(wide[0])));
    if (wn > 0) {
        int un = WideCharToMultiByte(CP_UTF8, 0, wide, -1, dst, (int)cap,
                                     nullptr, nullptr);
        if (un > 0) return;
    }
#endif
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 逐级创建父目录。模块的 CREATE/OPEN 不自动建父目录。 */
static void make_parents(FsCall fs, int drive, char *path) {
    for (size_t i = 1; path[i]; i++) {
        if (path[i] != '\\' && path[i] != '/') continue;
        char saved = path[i];
        path[i] = '\0';
        int64_t r = fs(SFS_OP_MKDIR, drive, (int64_t)(uintptr_t)path, 0, 0);
        if (r == 0) {
            std::fprintf(stderr, "mkfs2: mkdir '%s' returned 0 (may already exist)\n", path);
        }
        path[i] = saved;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: mkfs2 <image.img> <module.bin> <src>=<dest> [...]\n"
            "example: mkfs2 build\\saylter-ide.img build\\slyzfs2.bin "
            "samples\\hello.slyz=C:\\system\\apps\\hello.slyz\n");
        return 1;
    }

    const char *img_path = argv[1];
    const char *mod_path = argv[2];

    g_img = fopen(img_path, "r+b");
    if (!g_img) {
        std::fprintf(stderr, "mkfs2: cannot open image '%s'\n", img_path);
        return 1;
    }
    _fseeki64(g_img, 0, SEEK_END);
    long long bytes = _ftelli64(g_img);
    if (bytes <= 0) {
        fclose(g_img);
        std::fprintf(stderr, "mkfs2: image is empty\n");
        return 1;
    }
    g_sectors = (uint32_t)(bytes / 512LL);

    uint32_t mod_size = 0;
    unsigned char *mod = read_file(mod_path, &mod_size);
    if (!mod) {
        fclose(g_img);
        return 1;
    }

    const uintptr_t base = 0x04000000u;
    const size_t heap = 48u * 1024u * 1024u;
    unsigned char *mem = (unsigned char *)VirtualAlloc((void *)base,
                                                       heap + mod_size + 4096,
                                                       MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE);
    if (!mem) {
        std::free(mod);
        fclose(g_img);
        std::fprintf(stderr, "mkfs2: VirtualAlloc failed\n");
        return 1;
    }
    memset(mem, 0, heap + mod_size + 4096);
    memcpy(mem, mod, mod_size);
    std::free(mod);

    struct SlysFsApi api;
    api.version = SLYSFS_ABI;
    api.reserved = 0;
    api.disk_read = img_read;
    api.disk_write = img_write;
    api.total_sectors = img_sectors;
    api.log = img_log;
    api.flush = img_flush;

    void *(*start)(void *) = (void *(*)(void *))mem;
    FsCall fs = (FsCall)start(&api);
    if (!fs) {
        std::fprintf(stderr, "mkfs2: module entry is null\n");
        fclose(g_img);
        return 1;
    }

    if (fs(SFS_OP_FORMAT_DISK, 0, 0, 0, 0) != 1) {
        std::fprintf(stderr, "mkfs2: format failed (image too small?)\n");
        fclose(g_img);
        return 1;
    }
    if (fs(SFS_OP_MOUNT_ALL, 0, 0, 0, 0) == 0) {
        std::fprintf(stderr, "mkfs2: mount failed\n");
        fclose(g_img);
        return 1;
    }

    int failures = 0;
    for (int i = 3; i < argc; i++) {
        char spec[1024];
        size_t len = strlen(argv[i]);
        if (len >= sizeof(spec)) {
            std::fprintf(stderr, "mkfs2: argument too long\n");
            failures++;
            continue;
        }
        memcpy(spec, argv[i], len + 1);

        char *eq = strchr(spec, '=');
        if (!eq) {
            std::fprintf(stderr, "mkfs2: expected <src>=<dest>, got '%s'\n", spec);
            failures++;
            continue;
        }
        *eq = '\0';
        const char *src_path = spec;

        int drive = 0;
        const char *rel = nullptr;
        drive_and_path(eq + 1, &drive, &rel);

        char dest_utf8[1024];
        dest_to_utf8(rel, dest_utf8, sizeof(dest_utf8));
        make_parents(fs, drive, dest_utf8);

        uint32_t size = 0;
        unsigned char *data = read_file(src_path, &size);
        if (!data) {
            failures++;
            continue;
        }

        int64_t h = fs(SFS_OP_OPEN, drive, (int64_t)(uintptr_t)dest_utf8, 1, 0);
        if (h <= 0) {
            std::fprintf(stderr, "mkfs2: cannot create '%s' (open=%lld, drive=%d)\n",
                         dest_utf8, (long long)h, drive);
            std::free(data);
            failures++;
            continue;
        }

        int64_t written = fs(SFS_OP_WRITE, h, (int64_t)(uintptr_t)data, size, 0);
        fs(SFS_OP_CLOSE, h, 0, 0, 0);
        std::free(data);

        if (written != (int64_t)size) {
            std::fprintf(stderr, "mkfs2: short write for '%s' (%lld of %u)\n",
                         eq + 1, (long long)written, size);
            failures++;
            continue;
        }
        std::printf("mkfs2: %s -> %s (%u bytes)\n", src_path, eq + 1, size);
    }

    fs(SFS_OP_FLUSH, 0, 0, 0, 0);
    fclose(g_img);

    if (failures) {
        std::fprintf(stderr, "mkfs2: %d file(s) failed\n", failures);
        return 1;
    }
    return 0;
}
