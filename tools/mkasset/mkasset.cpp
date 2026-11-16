/*
 * mkasset.cpp - 生成系统预置壁纸与头像
 * ------------------------------------------------------------
 * 系统盘里的图片必须真实存在，用户在设置里才能立即看到效果。
 * 这里在构建期用程序生成一组 24 位 BMP，交给 mkfs 预置进
 * SlyzFS 的 system\wallpapers 与 system\avatars。
 *
 * 为什么生成而不是提交二进制素材：
 *   1. 仓库不需要保存体积较大的图片，构建产物可复现；
 *   2. 颜色与尺寸由代码决定，修改后重新构建即可，不会出现
 *      “素材和文档对不上”的情况；
 *   3. 生成器只依赖标准 C++，不引入图形库。
 *
 * 用法：
 *   mkasset <输出根目录>
 * 例如：
 *   mkasset build\assets
 * 结果：
 *   build\assets\wallpapers\aurora.bmp
 *   build\assets\wallpapers\dusk.bmp
 *   build\assets\wallpapers\forest.bmp
 *   build\assets\wallpapers\midnight.bmp
 *   build\assets\avatars\aurora.bmp
 *   build\assets\avatars\ember.bmp
 *   build\assets\avatars\leaf.bmp
 *   build\assets\avatars\slate.bmp
 */

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

Rgb rgb(uint32_t c) {
    Rgb v;
    v.r = static_cast<uint8_t>((c >> 16) & 0xFFu);
    v.g = static_cast<uint8_t>((c >> 8) & 0xFFu);
    v.b = static_cast<uint8_t>(c & 0xFFu);
    return v;
}

uint8_t clamp8(double v) {
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return static_cast<uint8_t>(v + 0.5);
}

Rgb lerp(Rgb a, Rgb b, double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    Rgb v;
    v.r = clamp8(a.r + (b.r - a.r) * t);
    v.g = clamp8(a.g + (b.g - a.g) * t);
    v.b = clamp8(a.b + (b.b - a.b) * t);
    return v;
}

Rgb add(Rgb base, Rgb light, double amount) {
    if (amount < 0.0) amount = 0.0;
    Rgb v;
    v.r = clamp8(base.r + light.r * amount);
    v.g = clamp8(base.g + light.g * amount);
    v.b = clamp8(base.b + light.b * amount);
    return v;
}

uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* ------------------------------------------------------------
 * 24 位 BMP 写入
 * ------------------------------------------------------------ */

bool write_bmp(const std::string &path, int w, int h,
               const std::vector<Rgb> &pixels) {
    if (w <= 0 || h <= 0) return false;
    if (static_cast<int>(pixels.size()) != w * h) return false;

    int row_bytes = ((w * 3) + 3) / 4 * 4;
    uint32_t data_size = static_cast<uint32_t>(row_bytes) * static_cast<uint32_t>(h);
    uint32_t file_size = 14u + 40u + data_size;

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "mkasset: cannot write '%s'\n", path.c_str());
        return false;
    }

    uint8_t header[54];
    for (int i = 0; i < 54; i++) header[i] = 0;

    header[0] = 'B';
    header[1] = 'M';

    auto put_u32 = [&](int off, uint32_t v) {
        header[off + 0] = static_cast<uint8_t>(v & 0xFFu);
        header[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        header[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        header[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    };
    auto put_u16 = [&](int off, uint16_t v) {
        header[off + 0] = static_cast<uint8_t>(v & 0xFFu);
        header[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    };

    put_u32(2, file_size);
    put_u32(10, 54);
    put_u32(14, 40);
    put_u32(18, static_cast<uint32_t>(w));
    put_u32(22, static_cast<uint32_t>(h));
    put_u16(26, 1);
    put_u16(28, 24);
    put_u32(30, 0);
    put_u32(34, data_size);
    put_u32(38, 2835);
    put_u32(42, 2835);
    put_u32(46, 0);
    put_u32(50, 0);

    if (std::fwrite(header, 1, 54, f) != 54) {
        std::fclose(f);
        return false;
    }

    std::vector<uint8_t> row(static_cast<size_t>(row_bytes), 0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const Rgb &c = pixels[static_cast<size_t>(y) * w + x];
            row[x * 3 + 0] = c.b;
            row[x * 3 + 1] = c.g;
            row[x * 3 + 2] = c.r;
        }
        if (std::fwrite(row.data(), 1, row.size(), f) != row.size()) {
            std::fclose(f);
            return false;
        }
    }

    std::fclose(f);
    return true;
}

/* ------------------------------------------------------------
 * 壁纸
 * ------------------------------------------------------------ */

std::vector<Rgb> make_aurora(int w, int h) {
    std::vector<Rgb> px(static_cast<size_t>(w) * h);

    Rgb top = rgb(0x050B16);
    Rgb bottom = rgb(0x0A2030);
    Rgb teal = rgb(0x2FD9C0);
    Rgb violet = rgb(0x6D5BFF);
    Rgb star = rgb(0xDCE8FF);

    for (int y = 0; y < h; y++) {
        double ty = static_cast<double>(y) / static_cast<double>(h - 1);

        for (int x = 0; x < w; x++) {
            double tx = static_cast<double>(x) / static_cast<double>(w - 1);

            Rgb c = lerp(top, bottom, ty);

            /* 两条缓慢起伏的极光带 */
            double band1 = 0.58 + 0.07 * std::sin(tx * 5.2 + ty * 2.0);
            double band2 = 0.36 + 0.05 * std::cos(tx * 3.1 - ty * 4.0);

            double d1 = ty - band1;
            double d2 = ty - band2;

            double glow1 = std::exp(-(d1 * d1) / (2.0 * 0.030 * 0.030));
            double glow2 = std::exp(-(d2 * d2) / (2.0 * 0.018 * 0.018));

            double pulse = 0.65 + 0.35 * std::sin(tx * 9.0 + 1.2);

            c = add(c, teal, glow1 * 0.55 * pulse);
            c = add(c, violet, glow2 * 0.42);

            /* 顶部稀疏星点 */
            if (ty < 0.55) {
                uint32_t hv = hash32(static_cast<uint32_t>(x) * 73856093u ^
                                     static_cast<uint32_t>(y) * 19349663u);
                if ((hv & 0xFFFu) < 3u) {
                    double fade = (0.55 - ty) / 0.55;
                    c = add(c, star, 0.55 * fade);
                }
            }

            px[static_cast<size_t>(y) * w + x] = c;
        }
    }

    return px;
}

std::vector<Rgb> make_dusk(int w, int h) {
    std::vector<Rgb> px(static_cast<size_t>(w) * h);

    Rgb sky_top = rgb(0x221436);
    Rgb sky_mid = rgb(0x6B2C52);
    Rgb sky_low = rgb(0xE07A45);
    Rgb sun_light = rgb(0xFFD9A0);
    Rgb hill = rgb(0x120B18);

    double horizon = 0.64;

    for (int y = 0; y < h; y++) {
        double ty = static_cast<double>(y) / static_cast<double>(h - 1);

        Rgb base;
        if (ty < horizon) {
            double t = ty / horizon;
            base = (t < 0.55) ? lerp(sky_top, sky_mid, t / 0.55)
                              : lerp(sky_mid, sky_low, (t - 0.55) / 0.45);
        } else {
            base = lerp(sky_low, rgb(0x3A1A2E), (ty - horizon) / (1.0 - horizon));
        }

        for (int x = 0; x < w; x++) {
            double tx = static_cast<double>(x) / static_cast<double>(w - 1);
            Rgb c = base;

            /* 太阳：轻微椭圆，靠近地平线时被山挡住 */
            double sx = 0.72;
            double sy = horizon - 0.085;
            double dx = (tx - sx) * 1.15;
            double dy = ty - sy;
            double dist2 = dx * dx + dy * dy;
            double sun = std::exp(-dist2 / (2.0 * 0.0075 * 0.0075));
            c = add(c, sun_light, sun * 0.95);
            c = add(c, sun_light, std::exp(-dist2 / (2.0 * 0.055 * 0.055)) * 0.22);

            /* 远山：两层正弦轮廓 */
            double ridge = horizon +
                           0.055 * std::sin(tx * 7.0 + 0.8) +
                           0.030 * std::sin(tx * 13.0 + 2.1);

            if (ty > ridge) {
                c = hill;
                /* 山脊边缘保留一点环境光 */
                double edge = ty - ridge;
                if (edge < 0.012) {
                    c = add(c, rgb(0x59304A), (0.012 - edge) / 0.012 * 0.6);
                }
            }

            /* 低空薄云 */
            double cloud = std::sin(tx * 6.0 + ty * 2.5);
            if (ty > 0.42 && ty < horizon && cloud > 0.72) {
                double a = (cloud - 0.72) / 0.28 * 0.10;
                c = add(c, sun_light, a);
            }

            px[static_cast<size_t>(y) * w + x] = c;
        }
    }

    return px;
}

std::vector<Rgb> make_forest(int w, int h) {
    std::vector<Rgb> px(static_cast<size_t>(w) * h);

    Rgb top = rgb(0x061310);
    Rgb bottom = rgb(0x0D2A21);
    Rgb shaft = rgb(0xBFE8B0);
    Rgb mist = rgb(0x9FD8C0);

    for (int y = 0; y < h; y++) {
        double ty = static_cast<double>(y) / static_cast<double>(h - 1);

        for (int x = 0; x < w; x++) {
            double tx = static_cast<double>(x) / static_cast<double>(w - 1);

            Rgb c = lerp(top, bottom, ty * 0.85);

            /* 斜向光柱：几条不同宽度的亮带 */
            for (int i = 0; i < 4; i++) {
                double offset = 0.10 + i * 0.22;
                double center = offset + (1.0 - ty) * 0.20;
                double d = tx - center;
                double width = 0.020 + i * 0.006;
                double beam = std::exp(-(d * d) / (2.0 * width * width));
                c = add(c, shaft, beam * 0.10 * (1.0 - ty * 0.55));
            }

            /* 底部雾层 */
            if (ty > 0.62) {
                double a = (ty - 0.62) / 0.38;
                c = add(c, mist, a * a * 0.16);
            }

            /* 缓慢的枝叶纹理：低频正弦叠加，制造层次感 */
            double leaf = std::sin(tx * 24.0 + ty * 9.0) *
                          std::sin(tx * 8.0 - ty * 14.0);
            c = add(c, rgb(0x1E4A34), (leaf > 0.0 ? leaf : 0.0) * 0.10);

            px[static_cast<size_t>(y) * w + x] = c;
        }
    }

    return px;
}

std::vector<Rgb> make_midnight(int w, int h) {
    std::vector<Rgb> px(static_cast<size_t>(w) * h);

    Rgb top = rgb(0x02040A);
    Rgb bottom = rgb(0x0A1226);
    Rgb star = rgb(0xE6F0FF);
    Rgb moon = rgb(0xD8E4FF);
    Rgb glow = rgb(0x4F7EC8);

    double mx = 0.78;
    double my = 0.28;
    double mr = 0.085;

    for (int y = 0; y < h; y++) {
        double ty = static_cast<double>(y) / static_cast<double>(h - 1);

        for (int x = 0; x < w; x++) {
            double tx = static_cast<double>(x) / static_cast<double>(w - 1);

            Rgb c = lerp(top, bottom, ty);

            uint32_t hv = hash32(static_cast<uint32_t>(x) * 2654435761u ^
                                 static_cast<uint32_t>(y) * 2246822519u);
            uint32_t density = (hv & 0x3FFu);
            if (density < 4u) {
                double bright = 0.35 + (hv >> 10 & 0x3F) / 63.0 * 0.55;
                double fade = 1.0 - ty * 0.55;
                c = add(c, star, bright * fade);
            }

            double dx = (tx - mx) * static_cast<double>(w) / static_cast<double>(h);
            double dy = ty - my;
            double dist = std::sqrt(dx * dx + dy * dy);

            c = add(c, glow, std::exp(-(dist * dist) / (2.0 * 0.10 * 0.10)) * 0.18);

            if (dist < mr) {
                double shade = 1.0 - (dist / mr) * 0.28;
                c.r = clamp8(moon.r * shade);
                c.g = clamp8(moon.g * shade);
                c.b = clamp8(moon.b * shade);

                /* 月面环形山：几处固定的暗斑 */
                double cx1 = dx + 0.020;
                double cy1 = dy - 0.018;
                if (cx1 * cx1 + cy1 * cy1 < 0.0008) {
                    c = add(c, rgb(0x8090B0), 0.35);
                }
                double cx2 = dx - 0.028;
                double cy2 = dy + 0.030;
                if (cx2 * cx2 + cy2 * cy2 < 0.0006) {
                    c = add(c, rgb(0x8090B0), 0.28);
                }
            }

            /* 底部一条极淡的地平线光 */
            if (ty > 0.90) {
                double a = (ty - 0.90) / 0.10;
                c = add(c, glow, a * 0.10);
            }

            px[static_cast<size_t>(y) * w + x] = c;
        }
    }

    return px;
}

/* ------------------------------------------------------------
 * 头像
 * ------------------------------------------------------------ */

std::vector<Rgb> make_avatar(int size, uint32_t inner_color,
                             uint32_t outer_color, int pattern) {
    std::vector<Rgb> px(static_cast<size_t>(size) * size);

    Rgb inner = rgb(inner_color);
    Rgb outer = rgb(outer_color);
    Rgb white = rgb(0xFFFFFF);

    double center = (size - 1) / 2.0;
    double max_dist = center;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double dx = (x - center) / max_dist;
            double dy = (y - center) / max_dist;
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist > 1.0) dist = 1.0;

            Rgb c = lerp(inner, outer, dist);

            /* 左上高光 */
            double hl = std::exp(-((dx + 0.35) * (dx + 0.35) +
                                   (dy + 0.42) * (dy + 0.42)) / 0.18);
            c = add(c, white, hl * 0.22);

            if (pattern == 0) {
                /* 同心环 */
                double ring = std::sin(dist * 18.0);
                if (ring > 0.45) {
                    c = add(c, white, (ring - 0.45) * 0.22);
                }
            } else if (pattern == 1) {
                /* 斜向光带 */
                double band = std::sin((dx + dy) * 7.0);
                if (band > 0.0) {
                    c = add(c, white, band * 0.10);
                }
            } else if (pattern == 2) {
                /* 中心亮斑 */
                c = add(c, white, std::exp(-(dist * dist) / 0.10) * 0.16);
            } else {
                /* 细网格 */
                int gx = x % 24;
                int gy = y % 24;
                if (gx < 2 || gy < 2) {
                    c = add(c, white, 0.10);
                }
            }

            px[static_cast<size_t>(y) * size + x] = c;
        }
    }

    return px;
}

bool save(const std::string &path, int w, int h,
          const std::vector<Rgb> &pixels) {
    if (!write_bmp(path, w, h, pixels)) return false;
    std::printf("mkasset: %s (%dx%d)\n", path.c_str(), w, h);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::string root = (argc >= 2) ? argv[1] : "build/assets";

    std::error_code ec;
    std::filesystem::create_directories(root + "/wallpapers", ec);
    if (ec) {
        std::fprintf(stderr, "mkasset: cannot create '%s/wallpapers'\n", root.c_str());
        return 1;
    }
    std::filesystem::create_directories(root + "/avatars", ec);
    if (ec) {
        std::fprintf(stderr, "mkasset: cannot create '%s/avatars'\n", root.c_str());
        return 1;
    }

    const int WW = 800;
    const int WH = 450;

    if (!save(root + "/wallpapers/aurora.bmp", WW, WH, make_aurora(WW, WH))) return 1;
    if (!save(root + "/wallpapers/dusk.bmp", WW, WH, make_dusk(WW, WH))) return 1;
    if (!save(root + "/wallpapers/forest.bmp", WW, WH, make_forest(WW, WH))) return 1;
    if (!save(root + "/wallpapers/midnight.bmp", WW, WH, make_midnight(WW, WH))) return 1;

    const int AS = 192;

    if (!save(root + "/avatars/aurora.bmp", AS, AS,
              make_avatar(AS, 0x3B82F6, 0x8B5CF6, 0))) return 1;
    if (!save(root + "/avatars/ember.bmp", AS, AS,
              make_avatar(AS, 0xF59E0B, 0xB91C1C, 1))) return 1;
    if (!save(root + "/avatars/leaf.bmp", AS, AS,
              make_avatar(AS, 0x34D399, 0x065F46, 2))) return 1;
    if (!save(root + "/avatars/slate.bmp", AS, AS,
              make_avatar(AS, 0x94A3B8, 0x1E293B, 3))) return 1;

    return 0;
}
