/*
 * mkfs.cpp - 把编译好的 SLYZ 程序预置进系统盘镜像
 * ------------------------------------------------------------
 * 构建流程用它把 samples/*.slyz 写进 saylter-ide.img 的 SlyzFS
 * 区域（LBA 8192 起），这样内核启动后 `run system\apps\hello.slyz`
 * 就能直接执行，不需要先在系统里手工输入程序。
 *
 * 用法：
 *   mkfs <image.img> <src.slyz>=<dest path> [...]
 *
 * 行为：
 *   - 镜像里已有有效 SlyzFS 就挂载，没有就格式化；
 *   - 逐级创建目标目录；
 *   - 覆盖写入程序文件（w 模式会先删除旧文件）。
 *
 * 磁盘读写直接作用于镜像文件的绝对 LBA，与内核 ata 驱动看到
 * 的扇区号一致（SlyzFS 内部的 SLYZ_FS_OFFSET 会自己加偏移）。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "disk_io.h"
#include "slyzfs.h"

static FILE    *g_img = nullptr;
static uint32_t g_sectors = 0;

bool slyz_disk_read(uint32_t lba, uint8_t *buf) {
    if (!g_img || !buf || lba >= g_sectors) return false;
    if (fseek(g_img, (long)lba * SLYZ_SECTOR_SIZE, SEEK_SET) != 0) return false;
    return fread(buf, 1, SLYZ_SECTOR_SIZE, g_img) == SLYZ_SECTOR_SIZE;
}

bool slyz_disk_write(uint32_t lba, const uint8_t *buf) {
    if (!g_img || !buf || lba >= g_sectors) return false;
    if (fseek(g_img, (long)lba * SLYZ_SECTOR_SIZE, SEEK_SET) != 0) return false;
    if (fwrite(buf, 1, SLYZ_SECTOR_SIZE, g_img) != SLYZ_SECTOR_SIZE) return false;
    fflush(g_img);
    return true;
}

uint32_t slyz_disk_total_sectors(void) {
    return g_sectors;
}

static uint8_t *read_file(const char *path, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mkfs: cannot open '%s'\n", path);
        return nullptr;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    long n = ftell(f);
    if (n <= 0) {
        fclose(f);
        fprintf(stderr, "mkfs: '%s' is empty\n", path);
        return nullptr;
    }
    rewind(f);

    uint8_t *buf = static_cast<uint8_t *>(malloc(static_cast<size_t>(n)));
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    if (fread(buf, 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) {
        free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    *out_size = static_cast<uint32_t>(n);
    return buf;
}

static void make_parent_dirs(slyzfs::SlyzFS &fs, const char *dest) {
    char path[512];
    size_t n = strlen(dest);
    if (n >= sizeof(path)) return;
    memcpy(path, dest, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (path[i] != '\\' && path[i] != '/') continue;
        char saved = path[i];
        path[i] = '\0';
        if (path[0] != '\0') fs.mkdir(path);
        path[i] = saved;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: mkfs <image.img> <src.slyz>=<dest path> [...]\n"
                "example: mkfs build\\saylter-ide.img samples\\hello.slyz=system\\apps\\hello.slyz\n");
        return 1;
    }

    g_img = fopen(argv[1], "r+b");
    if (!g_img) {
        fprintf(stderr, "mkfs: cannot open image '%s' (need read/write)\n", argv[1]);
        return 1;
    }

    if (fseek(g_img, 0, SEEK_END) != 0) {
        fclose(g_img);
        return 1;
    }
    long bytes = ftell(g_img);
    if (bytes <= 0) {
        fclose(g_img);
        fprintf(stderr, "mkfs: image is empty\n");
        return 1;
    }
    g_sectors = static_cast<uint32_t>(bytes / SLYZ_SECTOR_SIZE);

    slyzfs::SlyzFS fs;

    if (!fs.mount()) {
        fprintf(stderr, "mkfs: no filesystem, formatting %u sectors\n",
                static_cast<unsigned>(g_sectors));
        if (!fs.format(g_sectors)) {
            fprintf(stderr, "mkfs: format failed\n");
            fclose(g_img);
            return 1;
        }
        if (!fs.mount()) {
            fprintf(stderr, "mkfs: mount after format failed\n");
            fclose(g_img);
            return 1;
        }
    }

    int failures = 0;

    for (int i = 2; i < argc; i++) {
        char spec[1024];
        size_t len = strlen(argv[i]);
        if (len >= sizeof(spec)) {
            fprintf(stderr, "mkfs: argument too long\n");
            failures++;
            continue;
        }
        memcpy(spec, argv[i], len + 1);

        char *eq = strchr(spec, '=');
        if (!eq) {
            fprintf(stderr, "mkfs: expected <src>=<dest>, got '%s'\n", spec);
            failures++;
            continue;
        }
        *eq = '\0';
        const char *src_path = spec;
        const char *dest_path = eq + 1;

        uint32_t size = 0;
        uint8_t *data = read_file(src_path, &size);
        if (!data) {
            failures++;
            continue;
        }

        make_parent_dirs(fs, dest_path);

        slyzfs::FileHandle *fh = fs.open(dest_path, "w");
        if (!fh) {
            fprintf(stderr, "mkfs: cannot create '%s'\n", dest_path);
            free(data);
            failures++;
            continue;
        }

        size_t written = fs.write(fh, data, size);
        fs.close(fh);
        free(data);

        if (written != size) {
            fprintf(stderr, "mkfs: short write for '%s' (%u of %u)\n",
                    dest_path, static_cast<unsigned>(written), static_cast<unsigned>(size));
            failures++;
            continue;
        }

        printf("mkfs: %s -> %s (%u bytes)\n", src_path, dest_path,
               static_cast<unsigned>(size));
    }

    fs.unmount();
    fclose(g_img);

    if (failures) {
        fprintf(stderr, "mkfs: %d file(s) failed\n", failures);
        return 1;
    }
    return 0;
}
