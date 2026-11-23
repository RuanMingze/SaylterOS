/* ============================================================
 * ime.c - 全拼中文输入法（N 字连打 + 候选自适应）
 * ------------------------------------------------------------
 * 与旧版的两点本质区别：
 *
 *   1. N 字连打：不再只看最后两个音节。每次按键后对所有完整
 *      音节做一次 DP 分词（双字词优先、单字兜底），生成一条
 *      "整句候选"（例如 nihaonihaonihao -> 你好你好你好，
 *      caochangyingfeieryuetian -> 草长莺飞二月天），空格直接
 *      整句上屏；同时保留末两音节词与末音节单字候选，便于逐词
 *      选择。
 *
 *   2. 候选自适应：每次上屏把文本计入用户词频表，
 *      排序时用 count 做偏置。用户连续选择第二个候选（例如
 *      倪浩）后，它会稳定升到第一位。词频表按用户保存在
 *      home\<user>\ime.freq，写入节流并带容量上限。
 * ============================================================ */

#include "ime.h"
#include "ime_pinyin.h"
#include "ime_words.h"
#include "unicode.h"
#include "tty.h"
#include "auth.h"
#include "net.h"

#define IME_BUF_MAX       24
#define IME_CAND_MAX      64
#define IME_MAX_SYL       8
#define IME_TEXT_MAX      40
#define IME_FREQ_MAX      128
#define IME_FREQ_TEXT     40
#define IME_FREQ_SAVE_MS  1500

#define IME_SINGLE_COST   2200
#define IME_PHRASE_BONUS  1600
#define IME_BOOST_STEP    2000
#define IME_BOOST_MAX     60000

typedef struct {
    char     text[IME_TEXT_MAX];   /* UTF-8 候选文本 */
    int      used_syl;             /* 消耗的完整音节数；k+1 表示连尾前缀一起消耗 */
    int      tail_consumed;        /* 1 = 尾前缀被该候选消耗 */
    int      rank;                 /* 越小越靠前（已含词频偏置） */
} ime_cand_t;

static char       g_buf[IME_BUF_MAX + 1];
static int        g_len;
static int        g_page;
static int        g_cn;
static ime_cand_t g_cands[IME_CAND_MAX];
static int        g_cand_count;

/* ---------------- 用户词频 ---------------- */

typedef struct {
    char     text[IME_FREQ_TEXT];
    uint16_t count;
} ime_freq_t;

static ime_freq_t g_freq[IME_FREQ_MAX];
static int        g_freq_count;
static int        g_freq_dirty;
static uint32_t   g_freq_last_save;
static int        g_freq_loaded;
static char       g_freq_path[96];
static char       g_freq_io[4096];

extern void *slyzfs_open(const char *path, const char *mode);
extern int   slyzfs_read(void *fh, uint8_t *buf, int n);
extern int   slyzfs_write(void *fh, const uint8_t *buf, int n);
extern void  slyzfs_close(void *fh);

static int eq_text(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static void copy_text(char *dst, const char *src, int cap) {
    int i = 0;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int text_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void freq_build_path(void) {
    const struct AuthUser *u = auth_current();
    g_freq_path[0] = '\0';
    if (u && u->home[0]) {
        int p = 0;
        for (int i = 0; u->home[i] && p < (int)sizeof(g_freq_path) - 16; i++) {
            g_freq_path[p++] = u->home[i];
        }
        const char *tail = "\\ime.freq";
        for (int i = 0; tail[i] && p < (int)sizeof(g_freq_path) - 1; i++) {
            g_freq_path[p++] = tail[i];
        }
        g_freq_path[p] = '\0';
    } else {
        copy_text(g_freq_path, "etc\\ime.freq", (int)sizeof(g_freq_path));
    }
}

static int freq_index(const char *text) {
    for (int i = 0; i < g_freq_count; i++) {
        if (eq_text(g_freq[i].text, text)) return i;
    }
    return -1;
}

static int freq_boost(const char *text) {
    int idx = freq_index(text);
    if (idx < 0) return 0;
    int v = (int)g_freq[idx].count * IME_BOOST_STEP;
    if (v > IME_BOOST_MAX) v = IME_BOOST_MAX;
    return v;
}

static void freq_save(void) {
    if (!g_freq_dirty || !g_freq_path[0]) return;

    int p = 0;
    for (int i = 0; i < g_freq_count; i++) {
        if (g_freq[i].count == 0) continue;
        char num[8];
        int n = 0;
        uint32_t v = g_freq[i].count;
        char tmp[8];
        int tn = 0;
        if (v == 0) tmp[tn++] = '0';
        while (v > 0 && tn < (int)sizeof(tmp)) {
            tmp[tn++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (tn > 0 && n < (int)sizeof(num) - 1) num[n++] = tmp[--tn];
        num[n++] = ' ';
        for (int k = 0; k < n && p < (int)sizeof(g_freq_io) - 1; k++) {
            g_freq_io[p++] = num[k];
        }
        for (int k = 0; g_freq[i].text[k] && p < (int)sizeof(g_freq_io) - 2; k++) {
            g_freq_io[p++] = g_freq[i].text[k];
        }
        g_freq_io[p++] = '\n';
    }
    g_freq_io[p] = '\0';

    void *fh = slyzfs_open(g_freq_path, "w");
    if (fh) {
        slyzfs_write(fh, (const uint8_t *)g_freq_io, p);
        slyzfs_close(fh);
        g_freq_dirty = 0;
        g_freq_last_save = net_now_ms();
    }
}

static void freq_bump(const char *text) {
    if (!text || !text[0]) return;

    int idx = freq_index(text);
    if (idx < 0) {
        if (g_freq_count >= IME_FREQ_MAX) {
            /* 表满：淘汰计数最小的一条，给新词腾位置。 */
            int victim = 0;
            for (int i = 1; i < g_freq_count; i++) {
                if (g_freq[i].count < g_freq[victim].count) victim = i;
            }
            idx = victim;
        } else {
            idx = g_freq_count++;
        }
        copy_text(g_freq[idx].text, text, IME_FREQ_TEXT);
        g_freq[idx].count = 0;
    }

    if (g_freq[idx].count < 1000) g_freq[idx].count++;
    g_freq_dirty = 1;

    uint32_t now = net_now_ms();
    if (now - g_freq_last_save >= IME_FREQ_SAVE_MS) freq_save();
}

static void freq_load(void) {
    g_freq_count = 0;
    g_freq_dirty = 0;
    g_freq_last_save = net_now_ms();

    freq_build_path();
    if (!g_freq_path[0]) return;

    void *fh = slyzfs_open(g_freq_path, "r");
    if (!fh) return;

    int total = 0;
    while (total < (int)sizeof(g_freq_io) - 1) {
        int n = slyzfs_read(fh, (uint8_t *)g_freq_io + total,
                            (int)sizeof(g_freq_io) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    slyzfs_close(fh);
    g_freq_io[total] = '\0';

    int p = 0;
    while (p < total && g_freq_count < IME_FREQ_MAX) {
        while (p < total && (g_freq_io[p] == '\r' || g_freq_io[p] == '\n')) p++;
        if (p >= total) break;

        uint32_t count = 0;
        int digits = 0;
        while (p < total && g_freq_io[p] >= '0' && g_freq_io[p] <= '9') {
            count = count * 10u + (uint32_t)(g_freq_io[p] - '0');
            p++;
            digits++;
        }
        while (p < total && g_freq_io[p] == ' ') p++;
        if (!digits) {
            while (p < total && g_freq_io[p] != '\n') p++;
            continue;
        }

        int start = p;
        while (p < total && g_freq_io[p] != '\n' && g_freq_io[p] != '\r') p++;
        int n = p - start;
        if (n > 0) {
            if (n > IME_FREQ_TEXT - 1) n = IME_FREQ_TEXT - 1;
            for (int i = 0; i < n; i++) {
                g_freq[g_freq_count].text[i] = g_freq_io[start + i];
            }
            g_freq[g_freq_count].text[n] = '\0';
            g_freq[g_freq_count].count = (uint16_t)(count > 1000u ? 1000u : count);
            g_freq_count++;
        }
    }
    g_freq_dirty = 0;
}

/* ---------------- 音节表查询 ---------------- */

static int syl_exact_n(const char *p, int n) {
    for (int i = 0; i < IME_SYLLABLE_COUNT; i++) {
        const char *k = ime_syllables[i].key;
        int j = 0;
        while (j < n && k[j] && k[j] == p[j]) j++;
        if (j == n && k[j] == '\0') return i;
    }
    return -1;
}

static int syl_is_prefix_n(const char *p, int n) {
    for (int i = 0; i < IME_SYLLABLE_COUNT; i++) {
        const char *k = ime_syllables[i].key;
        int j = 0;
        while (j < n && k[j] && k[j] == p[j]) j++;
        if (j == n) return 1;
    }
    return 0;
}

static int split_syllables(const char *s, int n, uint16_t *ids, int max_ids,
                           int *tail_len) {
    if (tail_len) *tail_len = 0;
    if (n <= 0 || n > IME_BUF_MAX) return -1;

    char ok[IME_BUF_MAX + 1];
    int  from[IME_BUF_MAX + 1];
    int  sid[IME_BUF_MAX + 1];

    ok[0] = 1;
    from[0] = -1;
    sid[0] = -1;

    for (int i = 1; i <= n; i++) {
        ok[i] = 0;
        for (int j = 0; j < i; j++) {
            if (!ok[j]) continue;
            int len = i - j;
            if (len > 6) continue;
            int idx = syl_exact_n(s + j, len);
            if (idx >= 0) {
                ok[i] = 1;
                from[i] = j;
                sid[i] = idx;
                break;
            }
        }
    }

    int end = -1;
    if (ok[n]) {
        end = n;
    } else {
        for (int i = n - 1; i >= 0; i--) {
            if (!ok[i]) continue;
            int len = n - i;
            if (len <= 6 && syl_is_prefix_n(s + i, len)) {
                end = i;
                break;
            }
        }
    }
    if (end < 0) return -1;
    if (tail_len && end < n) *tail_len = n - end;

    int tmp[IME_MAX_SYL];
    int count = 0;
    int pos = end;
    while (pos > 0 && count < IME_MAX_SYL) {
        tmp[count++] = sid[pos];
        pos = from[pos];
    }
    int total = count;
    if (total > max_ids) total = max_ids;
    for (int i = 0; i < total; i++) {
        ids[i] = (uint16_t)tmp[count - 1 - i];
    }
    return total;
}

/* ---------------- 候选构造 ---------------- */

static int word_lower_bound(uint16_t s1, uint16_t s2) {
    int lo = 0;
    int hi = IME_WORD_COUNT;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        const ime_word_t *w = &ime_words[mid];
        if (w->s1 < s1 || (w->s1 == s1 && w->s2 < s2)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* 取该音节组合中词频最好的一条；返回 0 表示没有词条。 */
static int word_best(uint16_t s1, uint16_t s2, uint16_t *c1, uint16_t *c2,
                     int *score) {
    int pos = word_lower_bound(s1, s2);
    if (pos >= IME_WORD_COUNT) return 0;
    const ime_word_t *w = &ime_words[pos];
    if (w->s1 != s1 || w->s2 != s2) return 0;
    if (c1) *c1 = w->c1;
    if (c2) *c2 = w->c2;
    if (score) *score = w->score;
    return 1;
}

static int collect_prefix_syllables(const char *p, int n, uint16_t *out, int max) {
    int count = 0;
    for (int i = 0; i < IME_SYLLABLE_COUNT && count < max; i++) {
        const char *k = ime_syllables[i].key;
        int j = 0;
        while (j < n && k[j] && k[j] == p[j]) j++;
        if (j == n) out[count++] = (uint16_t)i;
    }
    return count;
}

static int append_cp(char *dst, int cap, int *p, uint32_t cp) {
    if (*p + 4 >= cap) return 0;
    int n = utf8_encode(cp, (uint8_t *)dst + *p);
    if (n <= 0) return 0;
    *p += n;
    dst[*p] = '\0';
    return 1;
}

/* 加入候选：同文本去重，保留排名更靠前的一条。 */
static void cand_add(const char *text, int used_syl, int tail_consumed, int base) {
    if (!text || !text[0]) return;
    int rank = base - freq_boost(text);

    for (int i = 0; i < g_cand_count; i++) {
        if (eq_text(g_cands[i].text, text)) {
            if (rank < g_cands[i].rank) {
                g_cands[i].rank = rank;
                g_cands[i].used_syl = used_syl;
                g_cands[i].tail_consumed = tail_consumed;
            }
            return;
        }
    }

    if (g_cand_count >= IME_CAND_MAX) return;
    copy_text(g_cands[g_cand_count].text, text, IME_TEXT_MAX);
    g_cands[g_cand_count].used_syl = used_syl;
    g_cands[g_cand_count].tail_consumed = tail_consumed;
    g_cands[g_cand_count].rank = rank;
    g_cand_count++;
}

/* 词表之外的常用专名/短语。通用词频表里没有"倪浩"这类人名，
 * 这里补一小张人工表，让同音候选学习有可切换的对象。 */
typedef struct {
    const char *s1;
    const char *s2;
    uint16_t c1;
    uint16_t c2;
    uint16_t score;
} ime_extra_t;

static const ime_extra_t g_extras[] = {
    { "ni", "hao", 0x502A, 0x6D69, 800 },    /* 倪浩 */
    { "li", "na",  0x8389, 0x5A1C, 900 },    /* 莉娜 */
    { "wei", "lai", 0x672A, 0x6765, 700 },   /* 未来 */
    { "shi", "jie", 0x4E16, 0x754C, 700 },   /* 世界 */
};

/* 把 (s1,s2) 的词条加入候选（最多 3 条），consumes_tail 表示是否吞掉尾前缀 */
static void add_word_group(uint16_t s1, uint16_t s2, int consumes_tail) {
    int pos = word_lower_bound(s1, s2);
    int added = 0;
    while (pos < IME_WORD_COUNT && added < 3) {
        const ime_word_t *w = &ime_words[pos];
        if (w->s1 != s1 || w->s2 != s2) break;

        char text[IME_TEXT_MAX];
        int p = 0;
        text[0] = '\0';
        append_cp(text, (int)sizeof(text), &p, w->c1);
        append_cp(text, (int)sizeof(text), &p, w->c2);

        int used = 2;
        cand_add(text, used, consumes_tail, (int)w->score);
        pos++;
        added++;
    }

    /* 人工补充词：按音节 key 匹配，避免依赖生成表的下标。 */
    for (int i = 0; i < (int)(sizeof(g_extras) / sizeof(g_extras[0])); i++) {
        int e1 = syl_exact_n(g_extras[i].s1, text_len(g_extras[i].s1));
        int e2 = syl_exact_n(g_extras[i].s2, text_len(g_extras[i].s2));
        if (e1 != (int)s1 || e2 != (int)s2) continue;

        char text[IME_TEXT_MAX];
        int p = 0;
        text[0] = '\0';
        append_cp(text, (int)sizeof(text), &p, g_extras[i].c1);
        append_cp(text, (int)sizeof(text), &p, g_extras[i].c2);
        cand_add(text, 2, consumes_tail, (int)g_extras[i].score);
    }
}

/* 多音节短语表：整句 DP 会优先匹配这些条目，避免把"草长莺飞"
 * 拆成同音字（造常应非）。文本直接存 UTF-8，音节序列在首次使用时
 * 由全拼切分器解析。 */
#define IME_PHRASE_MAX 16

typedef struct {
    const char *pinyin;
    const char *text;
    uint16_t    score;
    uint16_t    syl[IME_MAX_SYL];
    uint8_t     count;
    uint8_t     ready;      /* 0=未解析 1=可用 2=解析失败 */
} ime_phrase_t;

static ime_phrase_t g_phrases[IME_PHRASE_MAX] = {
    { "caochangyingfei", "草长莺飞", 120, {0}, 0, 0 },
    { "eryue",           "二月",     150, {0}, 0, 0 },
    { "chuntian",        "春天",     180, {0}, 0, 0 },
    { "shijie",          "世界",     200, {0}, 0, 0 },
    { "weilai",          "未来",     200, {0}, 0, 0 },
};
static int g_phrase_count = 5;

static void prepare_phrases(void) {
    for (int i = 0; i < g_phrase_count; i++) {
        if (g_phrases[i].ready) continue;
        int tail = 0;
        int n = split_syllables(g_phrases[i].pinyin,
                                text_len(g_phrases[i].pinyin),
                                g_phrases[i].syl, IME_MAX_SYL, &tail);
        if (n > 0 && tail == 0) {
            g_phrases[i].count = (uint8_t)n;
            g_phrases[i].ready = 1;
        } else {
            g_phrases[i].ready = 2;
        }
    }
}

/* 用 DP 生成整句候选：多音节短语 > 双字词 > 单字。返回排名基值。 */
static int build_phrase(const uint16_t *ids, int k) {
    if (k <= 0) return -1;

    prepare_phrases();

    int dp[IME_MAX_SYL + 1];
    int prev[IME_MAX_SYL + 1];
    uint16_t step_c1[IME_MAX_SYL + 1];
    uint16_t step_c2[IME_MAX_SYL + 1];
    int step_phrase[IME_MAX_SYL + 1];

    dp[0] = 0;
    prev[0] = -1;
    step_c1[0] = 0;
    step_c2[0] = 0;
    step_phrase[0] = -1;

    for (int i = 1; i <= k; i++) {
        const ime_syllable_t *e = &ime_syllables[ids[i - 1]];
        int best = dp[i - 1] + IME_SINGLE_COST;
        int bp = i - 1;
        uint16_t b1 = (e->count > 0) ? ime_char_pool[e->first] : 0x3F;
        uint16_t b2 = 0;
        int bph = -1;

        if (i >= 2) {
            uint16_t c1 = 0, c2 = 0;
            int score = 0;
            if (word_best(ids[i - 2], ids[i - 1], &c1, &c2, &score)) {
                int v = dp[i - 2] + score;
                if (v < best) {
                    best = v;
                    bp = i - 2;
                    b1 = c1;
                    b2 = c2;
                    bph = -1;
                }
            }
        }

        for (int pi = 0; pi < g_phrase_count; pi++) {
            ime_phrase_t *ph = &g_phrases[pi];
            if (ph->ready != 1 || ph->count < 2 || ph->count > i) continue;
            int match = 1;
            for (int t = 0; t < ph->count; t++) {
                if (ids[i - ph->count + t] != ph->syl[t]) {
                    match = 0;
                    break;
                }
            }
            if (!match) continue;
            int v = dp[i - ph->count] + ph->score;
            if (v < best) {
                best = v;
                bp = i - ph->count;
                b1 = 0;
                b2 = 0;
                bph = pi;
            }
        }

        dp[i] = best;
        prev[i] = bp;
        step_c1[i] = b1;
        step_c2[i] = b2;
        step_phrase[i] = bph;
    }

    uint16_t seq[IME_MAX_SYL * 4];
    int sn = 0;
    int i = k;
    while (i > 0) {
        if (step_phrase[i] >= 0) {
            const ime_phrase_t *ph = &g_phrases[step_phrase[i]];
            uint32_t cps[IME_MAX_SYL * 2];
            int cn = 0;
            const uint8_t *p = (const uint8_t *)ph->text;
            int left = text_len(ph->text);
            while (left > 0 && cn < (int)(sizeof(cps) / sizeof(cps[0]))) {
                uint32_t cp = 0;
                int used = utf8_decode(p, left, &cp);
                if (used <= 0) break;
                cps[cn++] = cp;
                p += used;
                left -= used;
            }
            while (cn > 0) seq[sn++] = (uint16_t)cps[--cn];
            i = prev[i];
        } else if (step_c2[i]) {
            seq[sn++] = step_c2[i];
            seq[sn++] = step_c1[i];
            i = prev[i];
        } else {
            seq[sn++] = step_c1[i];
            i = prev[i];
        }
    }

    char text[IME_TEXT_MAX];
    int p = 0;
    text[0] = '\0';
    while (sn > 0) {
        sn--;
        append_cp(text, (int)sizeof(text), &p, seq[sn]);
    }

    /* 整句候选加长度奖励：输入的音节越多，整句越应该排在
     * 双字词之前；但短输入（nihao）仍允许用户通过词频把同音词
     * 提到第一位。 */
    int base = dp[k] / k - IME_PHRASE_BONUS - k * 400;
    cand_add(text, k, 0, base);
    return base;
}

/* ---------------- 简拼（声母）匹配 ---------------- */

/* 当前候选是否来自简拼。简拼时整串字母对应候选词的所有音节，
 * 上屏直接吐出候选文本，不再做音节回填。 */
static int g_short;

#define IME_SHORT_KEEP 24

typedef struct {
    int      score;
    uint32_t c1;
    uint32_t c2;
} ime_short_hit_t;

static ime_short_hit_t g_short_hits[IME_SHORT_KEEP];
static int             g_short_hit_count;

/* 维护按 score 升序的前 K 条词条；表满后只接受更常用的词，
 * 避免候选上限被前面碰到的低频词占满。 */
static void short_hit_add(int score, uint32_t c1, uint32_t c2) {
    if (g_short_hit_count == IME_SHORT_KEEP &&
        score >= g_short_hits[IME_SHORT_KEEP - 1].score) {
        return;
    }

    int pos = g_short_hit_count;
    if (pos == IME_SHORT_KEEP) pos = IME_SHORT_KEEP - 1;
    while (pos > 0 && g_short_hits[pos - 1].score > score) {
        g_short_hits[pos] = g_short_hits[pos - 1];
        pos--;
    }
    g_short_hits[pos].score = score;
    g_short_hits[pos].c1 = c1;
    g_short_hits[pos].c2 = c2;
    if (g_short_hit_count < IME_SHORT_KEEP) g_short_hit_count++;
}

static int syllable_initial_is(uint16_t syl, char c) {
    return ime_syllables[syl].key[0] == c;
}

/* 简拼候选：输入串的每个字母代表一个音节的声母。
 *   dd -> 得到 / 到底 / 单独 ...（得到词频最好，排第一）
 *   nh -> 倪浩（人工补充表）
 *   ccyf -> 草长莺飞（短语表按首字母整串匹配）
 * 只在整串既不是完整音节、也不是某个音节前缀时启用，
 * 这样 zh / ch / sh 这类没打完的全拼不会被抢走。 */
static void build_short_candidates(void) {
    g_short = 1;
    int n = g_len;

    prepare_phrases();
    for (int pi = 0; pi < g_phrase_count; pi++) {
        ime_phrase_t *ph = &g_phrases[pi];
        if (ph->ready != 1 || ph->count != n) continue;
        int match = 1;
        for (int t = 0; t < n; t++) {
            if (ime_syllables[ph->syl[t]].key[0] != g_buf[t]) {
                match = 0;
                break;
            }
        }
        if (match) cand_add(ph->text, n, 0, (int)ph->score);
    }

    if (n != 2) return;   /* 其余长度只认短语表 */

    char i1 = g_buf[0];
    char i2 = g_buf[1];

    g_short_hit_count = 0;
    for (int w = 0; w < IME_WORD_COUNT; w++) {
        const ime_word_t *e = &ime_words[w];
        if (!syllable_initial_is(e->s1, i1)) continue;
        if (!syllable_initial_is(e->s2, i2)) continue;
        short_hit_add((int)e->score, e->c1, e->c2);
    }

    for (int i = 0; i < g_short_hit_count; i++) {
        char text[IME_TEXT_MAX];
        int p = 0;
        text[0] = '\0';
        append_cp(text, (int)sizeof(text), &p, g_short_hits[i].c1);
        append_cp(text, (int)sizeof(text), &p, g_short_hits[i].c2);
        cand_add(text, 2, 0, g_short_hits[i].score);
    }

    /* 人工补充词（倪浩、未来等）也按声母匹配。 */
    for (int i = 0; i < (int)(sizeof(g_extras) / sizeof(g_extras[0])); i++) {
        if (g_extras[i].s1[0] != i1 || g_extras[i].s2[0] != i2) continue;
        char text[IME_TEXT_MAX];
        int p = 0;
        text[0] = '\0';
        append_cp(text, (int)sizeof(text), &p, g_extras[i].c1);
        append_cp(text, (int)sizeof(text), &p, g_extras[i].c2);
        cand_add(text, 2, 0, (int)g_extras[i].score);
    }
}

/* 候选排序：按 rank 升序，稳定插入排序。 */
static void cand_sort(void) {
    for (int i = 1; i < g_cand_count; i++) {
        ime_cand_t tmp = g_cands[i];
        int j = i - 1;
        while (j >= 0 && g_cands[j].rank > tmp.rank) {
            g_cands[j + 1] = g_cands[j];
            j--;
        }
        g_cands[j + 1] = tmp;
    }
}

static void ime_validate(const char *where) {
    if (g_cand_count >= 0 && g_cand_count <= IME_CAND_MAX &&
        g_page >= 0 && g_page < 1024 &&
        g_len >= 0 && g_len <= IME_BUF_MAX) {
        return;
    }
    printk("[IME] state corrupt at %s: cand=%d page=%d len=%d\n",
           where, g_cand_count, g_page, g_len);
    g_cand_count = 0;
    g_page = 0;
    g_len = 0;
    g_buf[0] = '\0';
}

static void rebuild_candidates(void) {
    ime_validate("rebuild.enter");
    g_cand_count = 0;
    g_page = 0;
    g_short = 0;
    if (g_len <= 0) return;

    uint16_t ids[IME_MAX_SYL];
    int tail = 0;
    int k = split_syllables(g_buf, g_len, ids, IME_MAX_SYL, &tail);
    if (k < 0) {
        /* 整串既不是完整音节、也不是某个音节的前缀（dd、zg、nh），
         * 按简拼（每字声母）解释。 */
        int letters = 1;
        for (int i = 0; i < g_len; i++) {
            if (g_buf[i] < 'a' || g_buf[i] > 'z') {
                letters = 0;
                break;
            }
        }
        if (letters) build_short_candidates();
        cand_sort();
        ime_validate("rebuild.exit");
        return;
    }
    if (k == 0) return;   /* 还在打某个音节的前缀，等音节完整 */

    build_phrase(ids, k);

    if (tail > 0) {
        uint16_t pids[64];
        int pn = collect_prefix_syllables(g_buf + (g_len - tail), tail, pids, 64);
        for (int i = 0; i < pn; i++) {
            /* 未打完的音节总是接在最后一个完整音节之后：
             * zhongg -> zhong + g*, nihaom -> ni + hao + m*。 */
            add_word_group(ids[k - 1], pids[i], 1);
        }
    } else if (k >= 2) {
        add_word_group(ids[k - 2], ids[k - 1], 0);
    }

    const ime_syllable_t *entry = &ime_syllables[ids[k - 1]];
    for (int i = 0; i < (int)entry->count && g_cand_count < IME_CAND_MAX; i++) {
        char text[IME_TEXT_MAX];
        int p = 0;
        text[0] = '\0';
        append_cp(text, (int)sizeof(text), &p, ime_char_pool[entry->first + i]);
        cand_add(text, 1, 0, 4000 + i * 8);
    }

    cand_sort();
    ime_validate("rebuild.exit");
}

/* ---------------- 公共接口 ---------------- */

void ime_init(void) {
    g_len = 0;
    g_buf[0] = '\0';
    g_page = 0;
    g_cn = 0;
    g_cand_count = 0;
    g_short = 0;
}

void ime_reload_user(void) {
    freq_load();
}

void ime_flush(void) {
    if (g_freq_dirty) freq_save();
}

int ime_chinese_enabled(void) {
    return g_cn;
}

void ime_toggle_chinese(void) {
    g_cn = !g_cn;
    ime_reset();
}

void ime_set_chinese(int on) {
    g_cn = on ? 1 : 0;
    ime_reset();
}

void ime_reset(void) {
    g_len = 0;
    g_buf[0] = '\0';
    g_page = 0;
    g_cand_count = 0;
    g_short = 0;
}

const char *ime_composing(void) {
    return g_buf;
}

int ime_candidate_count(void) {
    return g_cand_count;
}

int ime_candidate_page(void) {
    return g_page;
}

int ime_candidate_pages(void) {
    if (g_cand_count <= 0) return 0;
    return (g_cand_count + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
}

int ime_candidate_text(int index, char *out, int cap) {
    if (!out || cap < 8) return 0;
    if (index < 0 || index >= IME_PAGE_SIZE) return 0;

    int pos = g_page * IME_PAGE_SIZE + index;
    if (pos < 0 || pos >= g_cand_count || pos >= IME_CAND_MAX) {
        if (pos < 0 || pos >= IME_CAND_MAX) {
            printk("[IME] candidate index out of range: pos=%d page=%d "
                   "cand=%d idx=%d\n", pos, g_page, g_cand_count, index);
            g_page = 0;
        }
        return 0;
    }

    copy_text(out, g_cands[pos].text, cap);
    return text_len(out);
}

uint16_t ime_punct_map(int ascii) {
    switch (ascii) {
        case ',':  return 0xFF0Cu;
        case '.':  return 0x3002u;
        case '?':  return 0xFF1Fu;
        case '!':  return 0xFF01u;
        case ';':  return 0xFF1Bu;
        case ':':  return 0xFF1Au;
        default:   return 0;
    }
}

/* 上屏第 index 个候选；候选之前未覆盖的音节按各自首选字输出。 */
static int commit_candidate(int index, char *out, int out_cap) {
    if (index < 0 || index >= IME_PAGE_SIZE) return IME_CONSUMED;
    int pos = g_page * IME_PAGE_SIZE + index;
    if (pos >= g_cand_count) return IME_CONSUMED;

    /* 简拼：整串字母就是这个候选词的声母，直接上屏。 */
    if (g_short) {
        const char *text = g_cands[pos].text;
        int p = 0;
        for (int i = 0; text[i] && p < out_cap - 1; i++) out[p++] = text[i];
        out[p] = '\0';
        freq_bump(text);
        ime_reset();
        return IME_COMMIT;
    }

    uint16_t ids[IME_MAX_SYL];
    int tail = 0;
    int k = split_syllables(g_buf, g_len, ids, IME_MAX_SYL, &tail);
    if (k <= 0) {
        ime_reset();
        return IME_CONSUMED;
    }

    int used = g_cands[pos].used_syl;
    if (used < 1) used = 1;
    if (used > k) used = k;
    int head = k - used;
    if (head < 0) head = 0;

    int keep_tail = (tail > 0 && !g_cands[pos].tail_consumed);

    char keep[IME_BUF_MAX + 1];
    int keep_len = 0;
    if (keep_tail) {
        keep_len = tail;
        for (int i = 0; i < tail; i++) keep[i] = g_buf[g_len - tail + i];
    }

    int p = 0;
    for (int i = 0; i < head; i++) {
        uint16_t cp = ime_char_pool[ime_syllables[ids[i]].first];
        int n = utf8_encode(cp, (uint8_t *)out + p);
        if (p + n + (int)text_len(g_cands[pos].text) + 4 > out_cap) break;
        p += n;
    }
    const char *text = g_cands[pos].text;
    for (int i = 0; text[i]; i++) {
        if (p >= out_cap - 1) break;
        out[p++] = text[i];
    }
    out[p] = '\0';

    freq_bump(text);

    ime_reset();
    if (keep_len > 0) {
        for (int i = 0; i < keep_len; i++) g_buf[i] = keep[i];
        g_buf[keep_len] = '\0';
        g_len = keep_len;
        rebuild_candidates();
    }
    return IME_COMMIT;
}

static int commit_cp(uint32_t cp, char *out, int out_cap) {
    if (!out || out_cap < 5) return IME_CONSUMED;
    int n = utf8_encode(cp, (uint8_t *)out);
    out[n] = '\0';
    return IME_COMMIT;
}

static int commit_raw(char *out, int out_cap) {
    if (g_len <= 0) {
        ime_reset();
        return IME_CONSUMED;
    }
    if (g_len + 1 > out_cap) {
        ime_reset();
        return IME_CONSUMED;
    }
    for (int i = 0; i < g_len; i++) out[i] = g_buf[i];
    out[g_len] = '\0';
    ime_reset();
    return IME_COMMIT;
}

int ime_key(int ch, char *out, int out_cap) {
    if (!out || out_cap < 32) return IME_PASS;
    ime_validate("key.enter");
    out[0] = '\0';

    if (!g_freq_loaded) {
        g_freq_loaded = 1;
        freq_load();
    }

    if (ch == 0xFE) {
        ime_toggle_chinese();
        return IME_CONSUMED;
    }

    if (!g_cn) return IME_PASS;

    if (ch == 0x1B) {
        if (g_len > 0) {
            ime_reset();
            return IME_CONSUMED;
        }
        return IME_PASS;
    }

    if (ch == '\b') {
        if (g_len > 0) {
            g_len--;
            g_buf[g_len] = '\0';
            rebuild_candidates();
            return IME_CONSUMED;
        }
        return IME_PASS;
    }

    if (ch == '\r' || ch == '\n') {
        if (g_len > 0) {
            return commit_raw(out, out_cap);
        }
        return IME_PASS;
    }

    if (ch == '-' || ch == 0xF8) {
        if (g_len > 0) {
            if (g_page > 0) g_page--;
            return IME_CONSUMED;
        }
        return IME_PASS;
    }

    if (ch == '=' || ch == 0xF9) {
        if (g_len > 0) {
            if ((g_page + 1) * IME_PAGE_SIZE < g_cand_count) g_page++;
            return IME_CONSUMED;
        }
        return IME_PASS;
    }

    if (ch >= '1' && ch <= '9') {
        int idx = ch - '1';
        if (g_len > 0 && idx < IME_PAGE_SIZE && idx < g_cand_count) {
            return commit_candidate(idx, out, out_cap);
        }
        return IME_PASS;
    }

    if (ch == ' ') {
        if (g_len > 0) {
            if (g_cand_count > 0) return commit_candidate(0, out, out_cap);
            return commit_raw(out, out_cap);
        }
        return IME_PASS;
    }

    char c = (char)ch;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    if (c >= 'a' && c <= 'z') {
        if (g_len >= IME_BUF_MAX) return IME_CONSUMED;
        g_buf[g_len] = c;
        g_buf[g_len + 1] = '\0';
        g_len++;
        rebuild_candidates();
        return IME_CONSUMED;
    }

    if (g_len == 0) {
        uint16_t cp = ime_punct_map(ch);
        if (cp) return commit_cp(cp, out, out_cap);
        return IME_PASS;
    }

    ime_reset();
    return IME_CONSUMED;
}
