/* ============================================================
 * ime.c - 全拼中文输入法（支持双字词）
 * ------------------------------------------------------------
 * 见 ime.h 的模型说明。实现上的几个要点：
 *   1. 音节切分用动态规划而不是贪心。贪心在 "xian" 这种串上会先
 *      吃成 "xia"+"n"，DP 能正确得到 "xian"，同时也能处理
 *      "nihaoma" 这种多音节连写。
 *   2. 候选表在按键后一次性重建（最多 64 项），查询接口只是读数组，
 *      候选栏每帧重绘不会反复做二分查找。
 *   3. 上屏时把"候选之前那些音节"按各自首选字一并输出。用户输入
 *      nihaoma 选"好吗"时，前面的 ni 会作为"你"一起上屏，符合
 *      连续输入的直觉。
 * ============================================================ */

#include "ime.h"
#include "ime_pinyin.h"
#include "ime_words.h"
#include "unicode.h"
#include "tty.h"

#define IME_BUF_MAX     24
#define IME_CAND_MAX    64
#define IME_WORD_PER_KEY 3
#define IME_WORD_SHOW    3
#define IME_MAX_SYL     8

typedef struct {
    uint16_t c1;
    uint16_t c2;    /* 0 表示单字候选 */
    uint16_t score; /* 词频权重，越小越靠前；单字候选恒为 0 */
} ime_cand_t;

static char      g_buf[IME_BUF_MAX + 1];
static int       g_len;
static int       g_page;
static int       g_cn;
static ime_cand_t g_cands[IME_CAND_MAX];
static int       g_cand_count;

/* ---------- 音节表查询 ---------- */

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

/* 整串切分。成功返回完整音节个数并把下标写入 ids；
 * tail_len 返回末尾"未打完的音节前缀"长度（0 表示正好切完）。
 * 完全无法解析返回 -1。 */
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
        /* 末尾允许是"某音节的前缀"：用户还没敲完最后一个音节。
         * 必须从 n-1 一直试到 0：整串只由一个前缀组成（例如刚敲下的
         * "n"、"zh"）时，切分点就在 0，漏掉它会把首字母当成非法输入
         * 丢掉，表现就是输入 nihao 只剩最后能成音的 "ao"。 */
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

/* ---------- 词表查询 ---------- */

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

/* 收集以 p[0..n) 为前缀的全部音节下标 */
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

/* 把 (s1, s2) 这一组词加入候选表 */
static void add_word_group(uint16_t s1, uint16_t s2) {
    int pos = word_lower_bound(s1, s2);
    int added = 0;
    while (pos < IME_WORD_COUNT && added < IME_WORD_PER_KEY) {
        const ime_word_t *w = &ime_words[pos];
        if (w->s1 != s1 || w->s2 != s2) break;
        if (g_cand_count >= IME_CAND_MAX) return;
        g_cands[g_cand_count].c1 = w->c1;
        g_cands[g_cand_count].c2 = w->c2;
        g_cands[g_cand_count].score = w->score;
        g_cand_count++;
        pos++;
        added++;
    }
}

/* ---------- 状态自检 ----------
 * g_cands 与 g_cand_count 在 BSS 里首尾相接，任何一次越界写都会让
 * 候选数变成一个巨大的值，随后 ime_candidate_text 就会拿野索引去读
 * 内存。这里在每个可能改写状态的入口做一次范围检查：一旦越界立即
 * 打印现场并复位，把"随机蓝屏"压缩成一条可定位的日志。 */
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

/* ---------- 候选重建 ---------- */

static void rebuild_candidates(void) {
    ime_validate("rebuild.enter");
    g_cand_count = 0;
    g_page = 0;
    if (g_len <= 0) return;

    uint16_t ids[IME_MAX_SYL];
    int tail = 0;
    int k = split_syllables(g_buf, g_len, ids, IME_MAX_SYL, &tail);
    if (k <= 0) return;

    int word_start = g_cand_count;

    if (tail > 0) {
        /* 末音节还没打完（如 zhongg 的 g）：把它展开成所有可能的音节，
         * 与前面的音节组合查词。用户敲 zhongg 时就能看到"中国"，
         * 而不是只有 zhong 的单字候选。 */
        uint16_t pids[64];
        int pn = collect_prefix_syllables(g_buf + (g_len - tail), tail, pids, 64);
        for (int i = 0; i < pn; i++) {
            if (k >= 2) add_word_group(ids[k - 2], pids[i]);
            else add_word_group(ids[k - 1], pids[i]);
        }
    } else if (k >= 2) {
        add_word_group(ids[k - 2], ids[k - 1]);
    }

    int word_end = g_cand_count;
    /* 跨多个潜在音节收集来的词，按词频权重统一排序 */
    for (int i = word_start + 1; i < word_end; i++) {
        ime_cand_t tmp = g_cands[i];
        int j = i - 1;
        while (j >= word_start && g_cands[j].score > tmp.score) {
            g_cands[j + 1] = g_cands[j];
            j--;
        }
        g_cands[j + 1] = tmp;
    }
    if (word_end - word_start > IME_WORD_SHOW) {
        /* 词候选之后才会追加单字候选，这里不存在"需要向前搬运的尾部数据"，
         * 直接把候选数收到保留数量即可。
         * 早期版本用 g_cands[i - excess] 搬运，当 excess 大于起点时会算
         * 出负下标，把 g_cands 前面的 g_len / g_page 一并覆盖——
         * 输入 zhongg 这类"完整音节 + 单字母前缀"时必然触发。 */
        g_cand_count = word_start + IME_WORD_SHOW;
    }

    /* 末音节 -> 单字 */
    const ime_syllable_t *entry = &ime_syllables[ids[k - 1]];
    for (int i = 0; i < (int)entry->count && g_cand_count < IME_CAND_MAX; i++) {
        g_cands[g_cand_count].c1 = ime_char_pool[entry->first + i];
        g_cands[g_cand_count].c2 = 0;
        g_cands[g_cand_count].score = 0;
        g_cand_count++;
    }

    ime_validate("rebuild.exit");
}

/* ---------- 公共接口 ---------- */

void ime_init(void) {
    g_len = 0;
    g_buf[0] = '\0';
    g_page = 0;
    g_cn = 0;
    g_cand_count = 0;
}

int ime_chinese_enabled(void) {
    return g_cn;
}

void ime_toggle_chinese(void) {
    g_cn = !g_cn;
    ime_reset();
}

void ime_reset(void) {
    g_len = 0;
    g_buf[0] = '\0';
    g_page = 0;
    g_cand_count = 0;
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
    /* 三重限制：候选数、数组容量、非负。
     * 只要 pos 有一个不满足就拒绝访问，绝不能让野索引读到 g_cands 之外。 */
    if (pos < 0 || pos >= g_cand_count || pos >= IME_CAND_MAX) {
        if (pos < 0 || pos >= IME_CAND_MAX) {
            printk("[IME] candidate index out of range: pos=%d page=%d "
                   "cand=%d idx=%d\n", pos, g_page, g_cand_count, index);
            g_page = 0;
        }
        return 0;
    }

    int n = utf8_encode(g_cands[pos].c1, (uint8_t *)out);
    if (g_cands[pos].c2) {
        n += utf8_encode(g_cands[pos].c2, (uint8_t *)out + n);
    }
    out[n] = '\0';
    return n;
}

uint16_t ime_punct_map(int ascii) {
    switch (ascii) {
        case ',':  return 0xFF0Cu;   /* ， */
        case '.':  return 0x3002u;   /* 。 */
        case '?':  return 0xFF1Fu;   /* ？ */
        case '!':  return 0xFF01u;   /* ！ */
        case ';':  return 0xFF1Bu;   /* ； */
        case ':':  return 0xFF1Au;   /* ： */
        default:   return 0;
    }
}

/* 上屏第 index 个候选；候选之前的音节按首选字一并输出 */
static int commit_candidate(int index, char *out, int out_cap) {
    if (index < 0 || index >= IME_PAGE_SIZE) return IME_CONSUMED;
    int pos = g_page * IME_PAGE_SIZE + index;
    if (pos >= g_cand_count) return IME_CONSUMED;

    uint16_t ids[IME_MAX_SYL];
    int tail = 0;
    int k = split_syllables(g_buf, g_len, ids, IME_MAX_SYL, &tail);
    if (k <= 0) {
        ime_reset();
        return IME_CONSUMED;
    }

    int is_word = (g_cands[pos].c2 != 0);
    int head = is_word ? k - 2 : k - 1;
    if (head < 0) head = 0;

    /* 选单字时保留尾部前缀：输入 zhongg 选"中"之后，剩下的 g 继续
     * 作为新拼音的开头；选词时尾前缀已经被词消耗，不再保留。 */
    char keep[IME_BUF_MAX + 1];
    int keep_len = 0;
    if (!is_word && tail > 0) {
        keep_len = tail;
        for (int i = 0; i < tail; i++) keep[i] = g_buf[g_len - tail + i];
    }

    int p = 0;
    for (int i = 0; i < head; i++) {
        uint16_t cp = ime_char_pool[ime_syllables[ids[i]].first];
        int n = utf8_encode(cp, (uint8_t *)out + p);
        if (p + n + 8 > out_cap) break;
        p += n;
    }
    p += utf8_encode(g_cands[pos].c1, (uint8_t *)out + p);
    if (is_word) {
        p += utf8_encode(g_cands[pos].c2, (uint8_t *)out + p);
    }
    out[p] = '\0';

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

/* 把缓冲里的原始字母原样上屏（放弃中文候选） */
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

    /* Ctrl+Space 由 keyboard.c 映射成 0xFE，任何时候都切模式 */
    if (ch == 0xFE) {
        ime_toggle_chinese();
        return IME_CONSUMED;
    }

    if (!g_cn) return IME_PASS;

    if (ch == 0x1B) {                  /* Esc：放弃当前拼音 */
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
            /* 回车 = 放弃中文候选，把已打的字母原样上屏。
             * 中文模式下敲 ehco 再回车，得到的是英文 ehco 而不是
             * 一堆候选字；想执行命令再按一次回车即可。 */
            return commit_raw(out, out_cap);
        }
        return IME_PASS;
    }

    if (ch == '-' || ch == 0xF8) {     /* 上一页 */
        if (g_len > 0) {
            if (g_page > 0) g_page--;
            return IME_CONSUMED;
        }
        return IME_PASS;
    }

    if (ch == '=' || ch == 0xF9) {     /* 下一页 */
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
            /* 没有候选（打的是英文或拼错）：空格也按原文上屏，
             * 否则拼音会一直卡在缓冲里，后续输入全部串在一起。 */
            return commit_raw(out, out_cap);
        }
        return IME_PASS;
    }

    char c = (char)ch;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    if (c >= 'a' && c <= 'z') {
        if (g_len >= IME_BUF_MAX) return IME_CONSUMED;

        /* 字母一律接受。即使当前串组不出音节（例如把 echo 打成 ehco），
         * 也不能吞键：用户可以退格修正，或者直接回车/空格把原文送出。
         * 候选是否出现由 rebuild_candidates 决定。 */
        g_buf[g_len] = c;
        g_buf[g_len + 1] = '\0';
        g_len++;
        rebuild_candidates();
        return IME_CONSUMED;
    }

    /* 标点：缓冲为空时直出中文标点 */
    if (g_len == 0) {
        uint16_t cp = ime_punct_map(ch);
        if (cp) return commit_cp(cp, out, out_cap);
        return IME_PASS;
    }

    /* 拼音进行中按了标点：丢弃拼音，按键本身也吞掉，避免误插入 */
    ime_reset();
    return IME_CONSUMED;
}
