/* ============================================================
 * ime.h - 全拼中文输入法（支持双字词）
 * ------------------------------------------------------------
 * 词库来自两个生成脚本：
 *   tools/gen_pinyin_table.py  403 个音节 -> 候选字（按字频排序）
 *   tools/gen_ime_words.py     CC-CEDICT 双字词 -> 12000 条常用词
 *
 * 输入模型是"整串音节切分"：
 *   缓冲里保存用户敲下的全部字母，每次按键后用动态规划判断整串能否
 *   切分成若干完整音节（末段允许是某音节的前缀）。能切分就继续累积，
 *   候选栏同时给出"末两音节组成的词"和"末音节的单字"。
 *   输入 nihao 得到 ni + hao，候选首位就是"你好"；空格或数字上屏。
 *
 * 本模块只管状态与候选数据，不碰像素：终端与 GUI 各自决定候选栏
 * 画在哪里，同一套逻辑同时服务命令行与编辑器。
 * ============================================================ */

#ifndef IME_H
#define IME_H

#include <stdint.h>

/* ime_key 的返回值 */
#define IME_PASS      0   /* 未消费，调用方按普通按键处理 */
#define IME_CONSUMED  1   /* 已消费，调用方忽略该键 */
#define IME_COMMIT    2   /* 已上屏，out 中是 UTF-8 文本 */

/* 每页候选数：终端一行放得下 5 个候选加序号 */
#define IME_PAGE_SIZE 5

void ime_init(void);

/* 登录后调用：按当前用户加载 home\<user>\ime.freq。
 * 词频表在首次按键时也会懒加载，这里显式调用只是让候选顺序
 * 在登录后的第一帧就正确。 */
void ime_reload_user(void);

/* 立即把脏词频写回磁盘（关机/注销前调用）。 */
void ime_flush(void);

/* 中文模式开关（Ctrl+Space） */
int  ime_chinese_enabled(void);
void ime_toggle_chinese(void);
void ime_set_chinese(int on);

/* 当前拼音缓冲（NUL 结尾），空串表示没有正在输入的音节 */
const char *ime_composing(void);

/* 候选总数、当前页、总页数 */
int  ime_candidate_count(void);
int  ime_candidate_page(void);
int  ime_candidate_pages(void);

/* 第 index 个候选的 UTF-8 文本（词两个字，单字一个字）。
 * 越界或没有候选时返回 0。 */
int  ime_candidate_text(int index, char *out, int cap);

/* 清空输入缓冲（切换焦点、提交命令时调用） */
void ime_reset(void);

/* 处理一个按键。ch 为 readline 给出的字符/键码。
 * out 至少 32 字节：上屏可能包含前几个音节的字，最长约 5 个汉字。 */
int ime_key(int ch, char *out, int out_cap);

/* 中文标点映射：返回全角码位，无映射返回 0。
 * 只映射命令行里几乎不用的六个标点，路径分隔符、引号、运算符
 * 一律保持 ASCII，避免中文模式下写不了命令。 */
uint16_t ime_punct_map(int ascii);

#endif /* IME_H */
