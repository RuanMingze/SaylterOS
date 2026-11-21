/* ============================================================
 * vc_find.c - 查找引擎实现
 * ============================================================ */

#include "vc_find.h"
#include "vc_alloc.h"
#include <string.h>

static char lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int ascii_word(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int match_at(const char *text, uint32_t n, uint32_t at,
                    const char *q, uint32_t qlen, VcFindOptions opt) {
    if (at + qlen > n) return 0;
    for (uint32_t i = 0; i < qlen; i++) {
        char a = text[at + i];
        char b = q[i];
        if (!opt.case_sensitive) {
            a = lower_ascii(a);
            b = lower_ascii(b);
        }
        if (a != b) return 0;
    }
    if (opt.whole_word) {
        if (at > 0 && ascii_word((unsigned char)text[at - 1])) return 0;
        if (at + qlen < n && ascii_word((unsigned char)text[at + qlen])) return 0;
    }
    return 1;
}

static char *dump_text(const VcText *t, uint32_t *len) {
    uint32_t n = vc_length(t);
    char *buf = (char *)vc_alloc(n ? n : 1);
    if (!buf) return 0;
    if (n) vc_copy_all(t, buf, n);
    *len = n;
    return buf;
}

int vc_find_next(const VcText *t, const char *q, uint32_t qlen,
                 VcFindOptions opt, uint32_t from, VcRange *out) {
    if (!t || !q || qlen == 0 || !out) return 0;
    uint32_t n = 0;
    char *buf = dump_text(t, &n);
    if (!buf) return 0;

    int found = 0;
    uint32_t at = from;
    if (at > n) at = n;
    for (uint32_t i = at; i + qlen <= n; i++) {
        if (match_at(buf, n, i, q, qlen, opt)) {
            out->start = i;
            out->len = qlen;
            found = 1;
            break;
        }
    }
    if (!found) {
        for (uint32_t i = 0; i + qlen <= at && i + qlen <= n; i++) {
            if (match_at(buf, n, i, q, qlen, opt)) {
                out->start = i;
                out->len = qlen;
                found = 1;
                break;
            }
        }
    }

    vc_free(buf);
    return found;
}

int vc_find_prev(const VcText *t, const char *q, uint32_t qlen,
                 VcFindOptions opt, uint32_t from, VcRange *out) {
    if (!t || !q || qlen == 0 || !out) return 0;
    uint32_t n = 0;
    char *buf = dump_text(t, &n);
    if (!buf) return 0;

    int found = 0;
    if (n < qlen) {
        vc_free(buf);
        return 0;
    }

    uint32_t last = n - qlen;
    uint32_t limit = from;
    if (limit > n) limit = n;
    uint32_t start = (limit > last) ? last : limit;

    for (int64_t at = (int64_t)start; at >= 0; at--) {
        if (match_at(buf, n, (uint32_t)at, q, qlen, opt)) {
            out->start = (uint32_t)at;
            out->len = qlen;
            found = 1;
            break;
        }
    }
    if (!found) {
        for (uint32_t at = last; at > start; at--) {
            if (match_at(buf, n, at, q, qlen, opt)) {
                out->start = at;
                out->len = qlen;
                found = 1;
                break;
            }
        }
    }

    vc_free(buf);
    return found;
}

int vc_find_all(const VcText *t, const char *q, uint32_t qlen,
                VcFindOptions opt, VcRange *out, int max) {
    if (!t || !q || qlen == 0 || !out || max <= 0) return 0;
    uint32_t n = 0;
    char *buf = dump_text(t, &n);
    if (!buf) return 0;

    int count = 0;
    for (uint32_t i = 0; i + qlen <= n && count < max; ) {
        if (match_at(buf, n, i, q, qlen, opt)) {
            out[count].start = i;
            out[count].len = qlen;
            count++;
            i += qlen;
        } else {
            i++;
        }
    }

    vc_free(buf);
    return count;
}
