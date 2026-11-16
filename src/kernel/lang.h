/* ============================================================
 * lang.h - 界面语言包
 * ------------------------------------------------------------
 * 设计：以"英文原文"作为键，运行时查表返回当前语言文本。
 * 这样调用点写成 lang_tr("Files")，未翻译的条目自动回退到英文，
 * 新增语言只需在 lang.c 的表里加一列，不需要改任何界面代码。
 * ============================================================ */

#ifndef LANG_H
#define LANG_H

#define LANG_EN 0
#define LANG_ZH 1
#define LANG_COUNT 2

/* 读取 system_cfg 的 lang 项并应用；应在 system_cfg_init 之后调用。 */
void lang_init(void);

int  lang_current(void);

/* 立即切换（不落盘）。持久化由调用方通过 system_cfg_set_lang 完成。 */
void lang_set(int id);

/* 英文原文 -> 当前语言文本；未收录时原样返回英文。 */
const char *lang_tr(const char *en);

#endif /* LANG_H */
