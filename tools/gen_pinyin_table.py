#!/usr/bin/env python3
# ============================================================
# gen_pinyin_table.py - 生成 SaylterOS 拼音输入法音节表
# ------------------------------------------------------------
# 数据来源：
#   1. Unicode 官方 Unihan 数据库（https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip）
#        Unihan_Readings.txt            kMandarin / kHanyuPinyin 提供读音
#        Unihan_DictionaryLikeData.txt  kGradeLevel 提供小学年级
#   2. 《现代汉语常用字表》单字频次（Jun Da 2004，源自教育部公开字表），
#      只用于给同音字排序，"的"优先于"得"就是靠它。
#
# 输出 src/kernel/ime_pinyin.c / ime_pinyin.h：
#   ime_syllables[]  403 个无声调音节，按字典序
#   ime_char_pool[]  按音节分组连续存放的 Unicode 码位
#
# 只收录 GB2312 一级汉字（3755 字），保证候选里出现的字一定有字模。
#
# 用法：
#   python tools/gen_pinyin_table.py [--unihan DIR] [--out PATH]
# ============================================================

import argparse
import io
import os
import re
import sys
import unicodedata
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KERNEL = os.path.join(ROOT, 'src', 'kernel')

UNIHAN_URL = 'https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip'
FREQ_URL = ('https://lingua.mtsu.edu/chinese-computing/statistics/char/'
            'listchangyong.php')

UMLAUT = set('\u00fc\u01d6\u01d8\u01da\u01dc')   # ü ǖ ǘ ǚ ǜ


def to_syllable(text):
    """带声调拼音 -> 无声调字母串；ü 统一记作 v。"""
    out = []
    for ch in text:
        if ch in UMLAUT:
            out.append('v')
            continue
        for part in unicodedata.normalize('NFD', ch):
            if unicodedata.category(part) == 'Mn':
                continue
            low = part.lower()
            if 'a' <= low <= 'z':
                out.append(low)
    return ''.join(out)


def level1_hanzi():
    """GB2312 一级汉字（区 16..55）与它的区位顺序。"""
    order = {}
    for qu in range(16, 56):
        for wei in range(1, 95):
            try:
                ch = bytes([0xA0 + qu, 0xA0 + wei]).decode('gb2312')
            except UnicodeDecodeError:
                continue
            if len(ch) != 1:
                continue
            cp = ord(ch)
            if not (0x4E00 <= cp <= 0x9FFF):
                continue
            order[cp] = (qu - 16) * 94 + (wei - 1)
    return order


def ensure_unihan(folder):
    readings = os.path.join(folder, 'Unihan_Readings.txt')
    data = os.path.join(folder, 'Unihan_DictionaryLikeData.txt')
    if os.path.exists(readings) and os.path.exists(data):
        return readings, data
    print('Unihan data missing, downloading...')
    raw = urllib.request.urlopen(UNIHAN_URL, timeout=180).read()
    os.makedirs(folder, exist_ok=True)
    import tempfile
    with tempfile.NamedTemporaryFile(suffix='.zip', delete=False) as tmp:
        tmp.write(raw)
        tmp_path = tmp.name
    with zipfile.ZipFile(tmp_path) as zf:
        zf.extractall(folder)
    os.unlink(tmp_path)
    return readings, data


def load_grades(path):
    table = {}
    with io.open(path, encoding='utf-8') as fp:
        for line in fp:
            if not line.startswith('U+'):
                continue
            parts = line.rstrip('\n').split('\t')
            if len(parts) < 3 or parts[1] != 'kGradeLevel':
                continue
            try:
                table[int(parts[0][2:], 16)] = int(parts[2].strip())
            except ValueError:
                continue
    return table


def load_freq_ranks(cache_path):
    """字频排名：字频越高排名越靠前（1 开始）。"""
    ranks = {}
    if os.path.exists(cache_path):
        with io.open(cache_path, encoding='utf-8') as fp:
            for line in fp:
                parts = line.rstrip('\n').split('\t')
                if len(parts) >= 2:
                    ranks[int(parts[0])] = int(parts[1])
        return ranks

    print('word-frequency table missing, downloading...')
    raw = urllib.request.urlopen(FREQ_URL, timeout=120).read()
    text = raw.decode('gb2312', errors='ignore')
    best = {}
    for m in re.finditer(r'(\d+)\t([^\t<]+)\t(\d+)', text):
        chars = m.group(2).strip()
        count = int(m.group(3))
        for ch in chars:
            cp = ord(ch)
            if not (0x4E00 <= cp <= 0x9FFF):
                continue
            if best.get(cp, -1) < count:
                best[cp] = count
    ordered = sorted(best, key=lambda c: best[c], reverse=True)
    lines = []
    for i, cp in enumerate(ordered):
        ranks[cp] = i + 1
        lines.append(f'{cp}\t{i + 1}')
    with io.open(cache_path, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write('\n'.join(lines) + '\n')
    return ranks


def load_readings(path, hanzi):
    """cp -> set(syllables)，kMandarin 与 kHanyuPinyin 合并。"""
    result = {}
    with io.open(path, encoding='utf-8') as fp:
        for line in fp:
            if not line.startswith('U+'):
                continue
            parts = line.rstrip('\n').split('\t')
            if len(parts) < 3:
                continue
            try:
                cp = int(parts[0][2:], 16)
            except ValueError:
                continue
            if cp not in hanzi:
                continue
            field, value = parts[1], parts[2].strip()
            if field == 'kMandarin':
                pieces = value.split()
            elif field == 'kHanyuPinyin':
                pieces = []
                for seg in value.split():
                    colon = seg.find(':')
                    if colon < 0:
                        continue
                    pieces.extend(seg[colon + 1:].split(','))
            else:
                continue
            for piece in pieces:
                syl = to_syllable(piece)
                if 1 <= len(syl) <= 6 and syl.isalpha() and syl.isascii():
                    result.setdefault(cp, set()).add(syl)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--unihan', default=os.path.join(
        os.environ.get('TEMP', '.'), 'unihan'))
    ap.add_argument('--out', default=os.path.join(KERNEL, 'ime_pinyin.c'))
    ap.add_argument('--header', default=os.path.join(KERNEL, 'ime_pinyin.h'))
    args = ap.parse_args()

    readings_path, data_path = ensure_unihan(args.unihan)
    hanzi = level1_hanzi()
    print(f'GB2312 level-1 hanzi: {len(hanzi)}')

    grades = load_grades(data_path)
    print(f'kGradeLevel entries: {len(grades)}')

    freq_cache = os.path.join(os.environ.get('TEMP', '.'), 'zibiao_freq.tsv')
    freq = load_freq_ranks(freq_cache)
    print(f'word-frequency ranks: {len(freq)}')

    readings = load_readings(readings_path, hanzi)
    missing = [cp for cp in hanzi if cp not in readings]
    print(f'hanzi with readings: {len(readings)}  missing: {len(missing)}')

    buckets = {}
    for cp, syllables in readings.items():
        grade = grades.get(cp, 7)
        rank = freq.get(cp, 4000 + grade * 100 + hanzi[cp] // 8)
        for syl in syllables:
            buckets.setdefault(syl, []).append((rank, hanzi[cp], cp))
    for syl in buckets:
        buckets[syl].sort()

    keys = sorted(buckets)
    pool = []
    entries = []
    for key in keys:
        first = len(pool)
        for _rank, _gb, cp in buckets[key]:
            pool.append(cp)
        entries.append((key, first, len(buckets[key])))

    print(f'syllables: {len(entries)}  pool: {len(pool)}')

    header = [
        '/* Auto-generated by tools/gen_pinyin_table.py - do not edit */',
        '/* Reading data derived from the Unicode Unihan database. */',
        '#ifndef IME_PINYIN_H',
        '#define IME_PINYIN_H',
        '',
        '#include <stdint.h>',
        '',
        f'#define IME_SYLLABLE_COUNT {len(entries)}',
        f'#define IME_POOL_COUNT     {len(pool)}',
        '#define IME_KEY_MAX        8',
        '',
        'typedef struct {',
        '    char     key[IME_KEY_MAX];',
        '    uint16_t first;',
        '    uint16_t count;',
        '} ime_syllable_t;',
        '',
        'extern const ime_syllable_t ime_syllables[IME_SYLLABLE_COUNT];',
        'extern const uint16_t ime_char_pool[IME_POOL_COUNT];',
        '',
        '#endif',
    ]
    with io.open(args.header, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write('\n'.join(header) + '\n')

    body = [
        '/* Auto-generated by tools/gen_pinyin_table.py - do not edit */',
        '/* Reading data derived from the Unicode Unihan database. */',
        '',
        '#include <stdint.h>',
        '#include "ime_pinyin.h"',
        '',
        'const ime_syllable_t ime_syllables[IME_SYLLABLE_COUNT] = {',
    ]
    for key, first, count in entries:
        body.append(f'    {{ "{key}", {first}, {count} }},')
    body.append('};')
    body.append('')
    body.append('const uint16_t ime_char_pool[IME_POOL_COUNT] = {')
    for i in range(0, len(pool), 16):
        chunk = ', '.join(f'0x{cp:04X}' for cp in pool[i:i + 16])
        body.append(f'    {chunk},')
    body.append('};')
    with io.open(args.out, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write('\n'.join(body) + '\n')

    print(f'generated {args.out} ({os.path.getsize(args.out)} bytes)')
    for probe in ('de', 'shi', 'ni', 'zhong', 'hao', 'ri', 'en', 'nin'):
        if probe not in buckets:
            continue
        chars = ''.join(chr(cp) for _r, _g, cp in buckets[probe][:10])
        print(f'  probe {probe} -> {chars}')


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    main()
