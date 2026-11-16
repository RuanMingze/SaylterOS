/*
 * mkslys.cpp - 为裸机驱动镜像生成 .slys 头部
 * ------------------------------------------------------------
 * 输入是链接脚本产出的原始二进制（从固定加载地址开始），输出
 * 是带 64 字节头部的 .slys。头部字段与 src/include/slys.h 完全
 * 一致，校验使用 FNV-1a。
 *
 * 用法：
 *   mkslys <input.bin> <output.slys> <name> <load_addr>
 *          <entry_offset> <bss_size>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(int argc, char **argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                     "usage: mkslys <in.bin> <out.slys> <name> <load> "
                     "<entry_off> <bss_size>\n");
        return 1;
    }

    const char *in_path = argv[1];
    const char *out_path = argv[2];
    const char *name = argv[3];
    uint32_t load = (uint32_t)std::strtoul(argv[4], nullptr, 0);
    uint32_t entry = (uint32_t)std::strtoul(argv[5], nullptr, 0);
    uint32_t bss = (uint32_t)std::strtoul(argv[6], nullptr, 0);

    FILE *in = std::fopen(in_path, "rb");
    if (!in) {
        std::fprintf(stderr, "mkslys: cannot open %s\n", in_path);
        return 1;
    }
    std::fseek(in, 0, SEEK_END);
    long n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(in);
        std::fprintf(stderr, "mkslys: empty image\n");
        return 1;
    }

    uint8_t *data = (uint8_t *)std::malloc((size_t)n);
    if (!data || std::fread(data, 1, (size_t)n, in) != (size_t)n) {
        std::fclose(in);
        std::free(data);
        std::fprintf(stderr, "mkslys: read failed\n");
        return 1;
    }
    std::fclose(in);

    uint8_t hdr[64];
    std::memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'S'; hdr[1] = 'L'; hdr[2] = 'Y'; hdr[3] = 'S';
    hdr[4] = 'D'; hdr[5] = 'R'; hdr[6] = 'V'; hdr[7] = 0;

    put16(hdr + 8, 1);
    put16(hdr + 10, 64);
    put32(hdr + 12, load);
    put32(hdr + 16, (uint32_t)n);
    put32(hdr + 20, bss);
    put32(hdr + 24, entry);
    put32(hdr + 28, 0);
    put32(hdr + 32, fnv1a(data, (size_t)n));
    put32(hdr + 36, 0);

    size_t k = 0;
    while (name[k] && k < 23) {
        hdr[40 + k] = (uint8_t)name[k];
        k++;
    }

    FILE *out = std::fopen(out_path, "wb");
    if (!out) {
        std::free(data);
        std::fprintf(stderr, "mkslys: cannot create %s\n", out_path);
        return 1;
    }
    bool ok = std::fwrite(hdr, 1, sizeof(hdr), out) == sizeof(hdr) &&
              std::fwrite(data, 1, (size_t)n, out) == (size_t)n;
    std::fclose(out);
    std::free(data);

    if (!ok) {
        std::fprintf(stderr, "mkslys: write failed\n");
        return 1;
    }

    std::printf("mkslys: %s (%ld bytes, bss %u, entry 0x%X)\n",
                out_path, n, bss, entry);
    return 0;
}
