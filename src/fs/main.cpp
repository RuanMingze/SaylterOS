/*
 * main.cpp —— SlyzFS 宿主端测试程序
 *
 * 用一块 256MB 内存模拟磁盘（524288 sectors），跑完整的格式化 → 挂载 →
 * 创建目录/文件 → 写入 → 读取 → 删除 → 卸载 → 重新挂载 → 再验证 流程。
 *
 * 编译：
 *   g++ -std=c++17 -O2 -o slyzfs_test main.cpp slyzfs.cpp
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

/* disk_io.h 需要我们实现这两个函数。 */
#include "disk_io.h"

static constexpr uint32_t DISK_TOTAL_SECTORS = 524288; /* 256MB */

static uint8_t* g_disk = nullptr;

static void init_disk() {
    g_disk = new uint8_t[(size_t)DISK_TOTAL_SECTORS * SLYZ_SECTOR_SIZE]();
}

static void free_disk() {
    delete[] g_disk;
    g_disk = nullptr;
}

bool slyz_disk_read(uint32_t lba, uint8_t* buf) {
    if (!g_disk || lba >= DISK_TOTAL_SECTORS) {
        std::fprintf(stderr, "[disk] read OOB: lba=%u total=%u\n", lba, DISK_TOTAL_SECTORS);
        return false;
    }
    std::memcpy(buf, g_disk + (size_t)lba * SLYZ_SECTOR_SIZE, SLYZ_SECTOR_SIZE);
    return true;
}

bool slyz_disk_write(uint32_t lba, const uint8_t* buf) {
    if (!g_disk || lba >= DISK_TOTAL_SECTORS) {
        std::fprintf(stderr, "[disk] write OOB: lba=%u total=%u\n", lba, DISK_TOTAL_SECTORS);
        return false;
    }
    std::memcpy(g_disk + (size_t)lba * SLYZ_SECTOR_SIZE, buf, SLYZ_SECTOR_SIZE);
    return true;
}

uint32_t slyz_disk_total_sectors() { return DISK_TOTAL_SECTORS; }

/* ======================= 测试 ======================= */

#include "slyzfs.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { g_pass++; std::printf("  [PASS] %s\n", #expr); } \
    else      { g_fail++; std::printf("  [FAIL] %s\n", #expr); } \
} while(0)

static void test_format_mount() {
    std::printf("\n=== Test 1: format + mount ===\n");
    slyzfs::SlyzFS fs;
    CHECK(fs.format(DISK_TOTAL_SECTORS));
    CHECK(fs.mount());
    fs.debug_print_fat();
    fs.debug_print_root();
}

static void test_mkdir() {
    std::printf("\n=== Test 2: mkdir ===\n");
    slyzfs::SlyzFS fs;
    CHECK(fs.mount());
    CHECK(fs.mkdir("slyz"));
    CHECK(fs.mkdir("slyz\\sys"));
    CHECK(fs.mkdir("slyz\\home"));
    CHECK(fs.mkdir("slyz\\home\\ruanftrix"));

    /* 重名应该失败 */
    CHECK(!fs.mkdir("slyz"));

    /* 列根目录 */
    fs.debug_print_root();
    fs.debug_print_fat();

    /* 子目录也能列 */
    slyzfs::DirEntry sub[64];
    int n = fs.list_dir("slyz", sub, 64);
    CHECK(n == 2); /* sys + home */
    std::printf("  slyz\\: %d entries\n", n);
    fs.unmount();
}

static void test_file_write_read() {
    std::printf("\n=== Test 3: file create/write/read ===\n");
    slyzfs::SlyzFS fs;
    CHECK(fs.mount());

    /* 创建文件 */
    CHECK(fs.create("slyz\\sys\\boot.slyz"));

    /* 打开写 */
    slyzfs::FileHandle* fh = fs.open("slyz\\sys\\boot.slyz", "w");
    CHECK(fh != nullptr);

    const char* hello = "Hello from SlyzFS! This is a test file.\n";
    size_t hlen = std::strlen(hello);
    size_t wn = fs.write(fh, reinterpret_cast<const uint8_t*>(hello), hlen);
    CHECK(wn == hlen);

    fs.close(fh);

    /* 再打开读 */
    fh = fs.open("slyz\\sys\\boot.slyz", "r");
    CHECK(fh != nullptr);
    uint8_t buf[256];
    std::memset(buf, 0, sizeof(buf));
    size_t rn = fs.read(fh, buf, sizeof(buf) - 1);
    CHECK(rn == hlen);
    CHECK(std::memcmp(buf, hello, hlen) == 0);
    std::printf("  content: %s", (char*)buf);
    fs.close(fh);

    fs.unmount();
}

static void test_persistence() {
    std::printf("\n=== Test 4: persistence (unmount → remount) ===\n");
    slyzfs::SlyzFS fs;
    CHECK(fs.mount());

    /* 验证之前写的文件还在 */
    slyzfs::FileHandle* fh = fs.open("slyz\\sys\\boot.slyz", "r");
    CHECK(fh != nullptr);
    uint8_t buf[256];
    std::memset(buf, 0, sizeof(buf));
    size_t rn = fs.read(fh, buf, sizeof(buf) - 1);
    CHECK(rn == 40); /* "Hello from SlyzFS! This is a test file.\n" */
    std::printf("  content: %s", (char*)buf);
    fs.close(fh);

    /* stat */
    slyzfs::DirEntry* de = fs.stat("slyz\\sys\\boot.slyz");
    CHECK(de != nullptr);
    CHECK(de->type == slyzfs::DIR_FILE);

    fs.debug_print_root();
    fs.debug_print_fat();
    fs.unmount();
}

static void test_large_file() {
    std::printf("\n=== Test 5: large file (cross-cluster write) ===\n");
    slyzfs::SlyzFS fs;
    CHECK(fs.mount());

    CHECK(fs.create("big.bin"));
    slyzfs::FileHandle* fh = fs.open("big.bin", "w");
    CHECK(fh != nullptr);

    /* 写 10KB，超过一个簇（2KB）触发 FAT 链扩展 */
    uint8_t pattern[1024];
    for (int i = 0; i < 1024; i++) pattern[i] = (uint8_t)(i & 0xFF);

    size_t total = 0;
    for (int k = 0; k < 10; k++) {
        size_t wn = fs.write(fh, pattern, 1024);
        total += wn;
    }
    CHECK(total == 10 * 1024);
    std::printf("  wrote %zu bytes\n", total);

    fs.close(fh);

    /* 重新读验证 */
    fh = fs.open("big.bin", "r");
    CHECK(fh != nullptr);
    uint8_t verify[1024];
    for (int k = 0; k < 10; k++) {
        size_t rn = fs.read(fh, verify, 1024);
        CHECK(rn == 1024);
        CHECK(std::memcmp(verify, pattern, 1024) == 0);
    }
    fs.close(fh);

    fs.debug_print_fat();
    fs.unmount();
}

static void test_remove() {
    std::printf("\n=== Test 6: remove file ===\n");
    slyzfs::SlyzFS fs;
    CHECK(fs.mount());

    CHECK(fs.remove("big.bin"));
    CHECK(fs.stat("big.bin") == nullptr);

    /* rmdir 需要空目录 */
    CHECK(fs.rmdir("slyz\\sys") == false); /* 里面有 boot.slyz */
    CHECK(fs.remove("slyz\\sys\\boot.slyz"));
    CHECK(fs.rmdir("slyz\\sys"));

    fs.debug_print_root();
    fs.debug_print_fat();
    fs.unmount();
}

static void test_path_normalize() {
    std::printf("\n=== Test 7: path normalize ===\n");
    char a[] = "foo/bar//baz.txt";
    slyzfs::SlyzFS::normalize_path(a);
    CHECK(std::strcmp(a, "foo\\bar\\baz.txt") == 0);

    char b[] = "\\foo\\bar\\";
    slyzfs::SlyzFS::normalize_path(b);
    CHECK(std::strcmp(b, "\\foo\\bar") == 0);

    char parent[512], name[64];
    CHECK(slyzfs::SlyzFS::split_path("slyz\\sys\\boot.slyz", parent, name));
    CHECK(std::strcmp(parent, "slyz\\sys") == 0);
    CHECK(std::strcmp(name, "boot.slyz") == 0);

    CHECK(slyzfs::SlyzFS::split_path("readme.txt", parent, name));
    CHECK(parent[0] == '\0');
    CHECK(std::strcmp(name, "readme.txt") == 0);
}

int main() {
    std::printf("SlyzFS 宿主端测试 (256MB 模拟磁盘)\n");
    std::printf("====================================\n");

    init_disk();

    test_format_mount();
    test_path_normalize();
    test_mkdir();
    test_file_write_read();
    test_persistence();
    test_large_file();
    test_remove();

    free_disk();

    std::printf("\n====================================\n");
    std::printf("  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    std::printf("====================================\n");

    return g_fail > 0 ? 1 : 0;
}
