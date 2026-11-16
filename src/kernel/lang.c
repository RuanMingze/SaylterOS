/* ============================================================
 * lang.c - 界面语言包
 * ------------------------------------------------------------
 * 表以英文原文为键。查表用线性扫描：条目只有几十项，一次按键
 * 触发几次查询，二分查找带来的复杂度不值得。
 * 未收录的文本原样返回英文，保证"没翻译的地方不会变成空白"。
 * ============================================================ */

#include "lang.h"
#include "system_cfg.h"

typedef struct {
    const char *en;
    const char *zh;
} lang_entry_t;

/* ---------------- 语言包：英文原文 -> 简体中文 ---------------- */

static const lang_entry_t g_zh[] = {
    /* 桌面 / 任务栏 */
    { "Search",                         "搜索" },
    { "Apps",                           "应用" },
    { "Power",                          "电源" },
    { "Network",                        "网络" },

    /* 应用名与描述（键必须与 g_apps 表里的英文逐字一致） */
    { "Files",                          "文件" },
    { "Text Editor",                    "文本编辑器" },
    { "Settings",                       "设置" },
    { "Terminal",                       "终端" },
    { "Browser",                        "浏览器" },
    { "Task Manager",                   "任务管理器" },
    { "Calculator",                     "计算器" },
    { "Browse your home directory",     "浏览主目录" },
    { "Create and edit text files",     "创建与编辑文本文件" },
    { "Run shell commands in a window", "在窗口中运行 shell 命令" },
    { "Browse http pages (HTML + CSS)", "浏览 http 页面（HTML + CSS）" },
    { "Boot mode, clock and account",   "启动模式、时钟与账户" },
    { "CPU, memory, disk and processes","处理器、内存、磁盘与进程" },
    { "Arithmetic with divide-by-zero guard", "带除零保护的四则运算" },

    /* 电源菜单 */
    { "POWER",                          "电源" },
    { "Restart",                        "重启" },
    { "Shut Down",                      "关机" },
    { "Exit to Terminal",               "返回终端" },
    { "Cancel",                         "取消" },

    /* 登录 */
    { "Sign in",                        "登录" },
    { "Password",                       "密码" },
    { "Welcome",                        "欢迎" },
    { "Incorrect password",             "密码错误" },
    { "Press Enter to sign in",         "按回车登录" },

    /* 设置页 */
    { "BOOT MODE",                      "启动模式" },
    { "GRAPHICAL",                      "图形界面" },
    { "TEXT",                           "文本终端" },
    { "GRAPHICAL DESKTOP",              "图形桌面" },
    { "TEXT TERMINAL",                  "文本终端" },
    { "DATE & TIME",                    "日期与时间" },
    { "TIMEZONE",                       "时区" },
    { "CLOCK",                          "时钟" },
    { "HOUR",                           "小时" },
    { "MIN",                            "分钟" },
    { "LANGUAGE",                       "语言" },
    { "Config: etc\\system.cfg",        "配置：etc\\system.cfg" },
    { "Session: ",                      "会话：" },
    { "   Home: ",                      "   主目录：" },
    { "Edit: nano etc\\system.cfg   (TTY)", "编辑：nano etc\\system.cfg   (终端)" },
    { "Reboot to apply boot mode.",     "重启后生效。" },
    { "Language switched.",             "语言已切换。" },
    { "Clock updated.",                 "时钟已调整。" },

    /* 文件管理器 */
    { "Up",                             "上一级" },
    { "Refresh",                        "刷新" },
    { "New Dir",                        "新建目录" },
    { "Create",                         "创建" },
    { "Open Home in Files",             "在文件管理器中打开主目录" },
    { "New folder",                     "新建文件夹" },
    { "New file",                       "新建文件" },
    { "Delete",                         "删除" },
    { "Open",                           "打开" },
    { "Name",                           "名称" },
    { "Size",                           "大小" },
    { "Empty folder",                   "空文件夹" },

    /* 编辑器 */
    { "Save",                           "保存" },
    { "Close",                          "关闭" },
    { "Line",                           "行" },
    { "Col",                            "列" },

    /* 终端 */
    { "Type 'help' for commands",       "输入 'help' 查看命令" },
};

#define LANG_ZH_COUNT ((int)(sizeof(g_zh) / sizeof(g_zh[0])))

static int g_lang = LANG_EN;

void lang_init(void) {
    g_lang = system_cfg_lang();
    if (g_lang < 0 || g_lang >= LANG_COUNT) g_lang = LANG_EN;
}

int lang_current(void) {
    return g_lang;
}

void lang_set(int id) {
    if (id < 0 || id >= LANG_COUNT) return;
    g_lang = id;
}

const char *lang_tr(const char *en) {
    if (!en) return "";
    if (g_lang == LANG_EN || g_lang <= 0) return en;

    for (int i = 0; i < LANG_ZH_COUNT; i++) {
        const char *a = g_zh[i].en;
        const char *b = en;
        int j = 0;
        while (a[j] && b[j] && a[j] == b[j]) j++;
        if (a[j] == '\0' && b[j] == '\0') return g_zh[i].zh;
    }
    return en;
}
