/* ============================================================
 * mkslyzd - 把裸机镜像与清单打包成 .slyzd
 * ------------------------------------------------------------
 * 用法：
 *   mkslyzd <image.bin> <manifest.txt> <out.slyzd>
 *
 * 清单指令（# 或 ; 开头为注释）：
 *   name    <模块名>
 *   org     <加载地址，支持 0x>
 *   entry   <入口偏移>
 *   export  <符号名> <偏移> <kind 0函数/1数据/2只读> <版本>
 *   import  <符号名> <槽位偏移> <kind> <版本>
 *   reloc   <偏移> <类型 1=ABS64/2=ABS32>
 *   dep     <模块名> <最低版本>
 *
 * 文件布局：64 字节头 -> 导出表 -> 导入表 -> 重定位表 ->
 * 依赖表 -> 4 字节对齐 -> 镜像。头里的 image_off 指向镜像。
 * ============================================================ */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "slyzlink.h"

static bool read_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size();
}

static std::string trim(const std::string &s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

static bool parse_u32(const std::string &s, uint32_t *out) {
    if (s.empty()) return false;
    char *end = 0;
    unsigned long v = strtoul(s.c_str(), &end, 0);
    if (!end || *end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

static bool copy_name(char *dst, size_t cap, const std::string &src) {
    if (src.empty() || src.size() >= cap) return false;
    memset(dst, 0, cap);
    memcpy(dst, src.data(), src.size());
    return true;
}

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: mkslyzd <image.bin> <manifest.txt> <out.slyzd>\n");
        return 1;
    }

    std::vector<uint8_t> image;
    if (!read_file(argv[1], image)) {
        fprintf(stderr, "mkslyzd: cannot read image %s\n", argv[1]);
        return 1;
    }

    SlyzModuleHeader h;
    memset(&h, 0, sizeof(h));
    h.magic[0] = SLYZD_MAGIC0;
    h.magic[1] = SLYZD_MAGIC1;
    h.magic[2] = SLYZD_MAGIC2;
    h.magic[3] = SLYZD_MAGIC3;
    h.magic[4] = SLYZD_MAGIC4;
    h.version = SLYZLINK_VERSION;
    h.flags = SLYZ_MOD_DYNAMIC;
    h.header_size = SLYZLINK_HEADER_SIZE;
    h.image_size = (uint32_t)image.size();

    std::vector<SlyzSymbol> exports;
    std::vector<SlyzImport> imports;
    std::vector<SlyzReloc> relocs;
    std::vector<SlyzDep> deps;

    FILE *mf = fopen(argv[2], "r");
    if (!mf) {
        fprintf(stderr, "mkslyzd: cannot read manifest %s\n", argv[2]);
        return 1;
    }

    char line[512];
    int lineno = 0;
    bool has_org = false;
    while (fgets(line, (int)sizeof(line), mf)) {
        lineno++;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;

        char op[32];
        char a[128], b[128], c[128], d[128];
        int n = sscanf(s.c_str(), "%31s %127s %127s %127s %127s", op, a, b, c, d);
        std::string w(op);

        if (w == "name") {
            if (n < 2 || !copy_name(h.name, sizeof(h.name), a)) {
                fprintf(stderr, "mkslyzd: bad name at line %d\n", lineno);
                return 1;
            }
        } else if (w == "org") {
            if (n < 2 || !parse_u32(a, &h.load_addr)) {
                fprintf(stderr, "mkslyzd: bad org at line %d\n", lineno);
                return 1;
            }
            has_org = true;
        } else if (w == "entry") {
            if (n < 2 || !parse_u32(a, &h.entry_offset)) {
                fprintf(stderr, "mkslyzd: bad entry at line %d\n", lineno);
                return 1;
            }
        } else if (w == "export") {
            SlyzSymbol sym;
            memset(&sym, 0, sizeof(sym));
            uint32_t off = 0, kind = 0, ver = 0;
            if (n < 5 || !copy_name(sym.name, sizeof(sym.name), a) ||
                !parse_u32(b, &off) || !parse_u32(c, &kind) || !parse_u32(d, &ver)) {
                fprintf(stderr, "mkslyzd: bad export at line %d\n", lineno);
                return 1;
            }
            sym.offset = off;
            sym.kind = (uint16_t)kind;
            sym.version = (uint16_t)ver;
            exports.push_back(sym);
        } else if (w == "import") {
            SlyzImport imp;
            memset(&imp, 0, sizeof(imp));
            uint32_t off = 0, kind = 0, ver = 0;
            if (n < 5 || !copy_name(imp.name, sizeof(imp.name), a) ||
                !parse_u32(b, &off) || !parse_u32(c, &kind) || !parse_u32(d, &ver)) {
                fprintf(stderr, "mkslyzd: bad import at line %d\n", lineno);
                return 1;
            }
            imp.slot_offset = off;
            imp.kind = (uint16_t)kind;
            imp.version = (uint16_t)ver;
            imports.push_back(imp);
        } else if (w == "reloc") {
            SlyzReloc r;
            memset(&r, 0, sizeof(r));
            uint32_t off = 0, type = 0;
            if (n < 3 || !parse_u32(a, &off) || !parse_u32(b, &type)) {
                fprintf(stderr, "mkslyzd: bad reloc at line %d\n", lineno);
                return 1;
            }
            r.offset = off;
            r.type = (uint8_t)type;
            relocs.push_back(r);
        } else if (w == "dep") {
            SlyzDep dep;
            memset(&dep, 0, sizeof(dep));
            uint32_t ver = 0;
            if (n < 3 || !copy_name(dep.name, sizeof(dep.name), a) ||
                !parse_u32(b, &ver)) {
                fprintf(stderr, "mkslyzd: bad dep at line %d\n", lineno);
                return 1;
            }
            dep.min_version = (uint16_t)ver;
            deps.push_back(dep);
        } else {
            fprintf(stderr, "mkslyzd: unknown directive '%s' at line %d\n", w.c_str(), lineno);
            return 1;
        }
    }
    fclose(mf);

    if (!has_org || h.name[0] == 0 || h.load_addr == 0) {
        fprintf(stderr, "mkslyzd: manifest must set name and org\n");
        return 1;
    }
    if (h.entry_offset >= h.image_size) {
        fprintf(stderr, "mkslyzd: entry offset outside image\n");
        return 1;
    }

    /* 导出/导入/重定位目标必须落在镜像内 */
    for (size_t i = 0; i < exports.size(); i++) {
        if (exports[i].offset >= h.image_size) {
            fprintf(stderr, "mkslyzd: export '%s' outside image\n", exports[i].name);
            return 1;
        }
    }
    for (size_t i = 0; i < imports.size(); i++) {
        if (imports[i].slot_offset + 8u > h.image_size) {
            fprintf(stderr, "mkslyzd: import '%s' slot outside image\n", imports[i].name);
            return 1;
        }
    }
    for (size_t i = 0; i < relocs.size(); i++) {
        uint32_t need = (relocs[i].type == SLYZ_RELOC_ABS64) ? 8u : 4u;
        if (relocs[i].offset + need > h.image_size) {
            fprintf(stderr, "mkslyzd: reloc outside image\n");
            return 1;
        }
    }

    uint32_t off = SLYZLINK_HEADER_SIZE;
    h.export_off = exports.empty() ? 0 : off;
    h.export_count = (uint32_t)exports.size();
    off += (uint32_t)(exports.size() * sizeof(SlyzSymbol));
    h.import_off = imports.empty() ? 0 : off;
    h.import_count = (uint32_t)imports.size();
    off += (uint32_t)(imports.size() * sizeof(SlyzImport));
    h.reloc_off = relocs.empty() ? 0 : off;
    h.reloc_count = (uint32_t)relocs.size();
    off += (uint32_t)(relocs.size() * sizeof(SlyzReloc));
    h.dep_off = deps.empty() ? 0 : off;
    h.dep_count = (uint32_t)deps.size();
    off += (uint32_t)(deps.size() * sizeof(SlyzDep));
    off = (off + 3u) & ~3u;
    h.image_off = off;
    h.checksum = fnv1a(image.data(), image.size());

    FILE *out = fopen(argv[3], "wb");
    if (!out) {
        fprintf(stderr, "mkslyzd: cannot write %s\n", argv[3]);
        return 1;
    }

    bool ok = true;
    ok = ok && fwrite(&h, 1, sizeof(h), out) == sizeof(h);
    if (!exports.empty()) ok = ok && fwrite(exports.data(), 1, exports.size() * sizeof(SlyzSymbol), out) == exports.size() * sizeof(SlyzSymbol);
    if (!imports.empty()) ok = ok && fwrite(imports.data(), 1, imports.size() * sizeof(SlyzImport), out) == imports.size() * sizeof(SlyzImport);
    if (!relocs.empty()) ok = ok && fwrite(relocs.data(), 1, relocs.size() * sizeof(SlyzReloc), out) == relocs.size() * sizeof(SlyzReloc);
    if (!deps.empty()) ok = ok && fwrite(deps.data(), 1, deps.size() * sizeof(SlyzDep), out) == deps.size() * sizeof(SlyzDep);
    while (ftell(out) < (long)h.image_off) {
        uint8_t zero = 0;
        if (fwrite(&zero, 1, 1, out) != 1) { ok = false; break; }
    }
    if (ok) ok = fwrite(image.data(), 1, image.size(), out) == image.size();
    fclose(out);

    if (!ok) {
        fprintf(stderr, "mkslyzd: write failed\n");
        return 1;
    }

    printf("mkslyzd: %s  load=0x%X image=%u exports=%u imports=%u relocs=%u deps=%u\n",
           h.name, h.load_addr, h.image_size,
           h.export_count, h.import_count, h.reloc_count, h.dep_count);
    return 0;
}
