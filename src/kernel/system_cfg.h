/* ============================================================
 * system_cfg.h - 全局系统配置（SlyzFS 上的 etc\system.cfg）
 * ------------------------------------------------------------
 * 设计目标：
 *   1. 用普通文本文件保存启动模式，用户可以直接 `nano etc\system.cfg`
 *      修改，和在 Linux 上编辑 /etc 下的配置是同一套心智模型；
 *   2. 解析器只认 `key=value`，`#` 开头为注释，未知键忽略，
 *      这样以后新增配置项不需要改动读取逻辑；
 *   3. 写入时保留文件头注释，只替换 boot.mode 行；
 *   4. 文件不存在时自动创建默认配置（boot.mode=tty），
 *      保证用户第一次 `nano etc\system.cfg` 看到的是一份可编辑的模板。
 * ============================================================ */

#ifndef SYSTEM_CFG_H
#define SYSTEM_CFG_H

#define SYSTEM_CFG_PATH "etc\\system.cfg"

/* 确保 etc 目录与配置文件存在并加载到内存。必须在 SlyzFS 挂载之后调用。 */
void system_cfg_init(void);

/* 启动模式：1 = GUI 登录/桌面，0 = TTY shell */
int system_cfg_boot_gui(void);

/* 写回启动模式。内部临时提权到 uid 0，普通用户也能通过 GUI 设置修改。 */
int system_cfg_set_boot_gui(int gui);

/* 时区偏移（整小时，-12 .. +14）。CMOS RTC 按 UTC 解释，
 * 桌面时钟显示 UTC + offset。默认 +8。 */
int system_cfg_timezone(void);

/* 写回时区偏移。会连同当前 boot.mode 一起重写整个配置文件。 */
int system_cfg_set_timezone(int hours);

/* 界面语言：0 = English，1 = 简体中文。默认 1。
 * 配置项写作 lang=zh / lang=en，下次启动生效。 */
int system_cfg_lang(void);
int system_cfg_set_lang(int id);

/* 桌面壁纸：SlyzFS 绝对路径，例如 system\wallpapers\aurora.bmp。
 * 空字符串表示使用内置渐变背景。修改后立即生效（桌面会整屏重绘）。 */
const char *system_cfg_wallpaper(void);
int system_cfg_set_wallpaper(const char *path);

/* 用户头像：SlyzFS 绝对路径。空字符串表示使用首字母渐变头像。 */
const char *system_cfg_avatar(void);
int system_cfg_set_avatar(const char *path);

#endif /* SYSTEM_CFG_H */
