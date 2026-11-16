#ifndef SHELL_H
#define SHELL_H

void shell_main(void);
void gui_main(void);

/* GUI 内嵌终端使用：执行一行命令（不含换行）。
 * 返回 1 = 已执行，0 = 命令不存在，2 = 用户请求退出会话。
 * 输出走 printk，可被 tty_set_output_hook 截获。 */
int shell_exec_line(char *line);

/* 当前 shell 提示符（随 cwd/用户变化，调用方不要长期持有） */
const char *shell_prompt_text(void);

/* 按当前 shell cwd 把用户输入解析成 SlyzFS 绝对路径。
 * GUI 终端的 nano 需要和 TTY 一致地解析相对路径。成功返回 1。 */
int shell_resolve_input(const char *in, char *out, int cap);

/* 非交互模式：GUI 内嵌终端在执行命令前置 1，结束后清 0。
 * 打开后 rm 需要显式 -f、read_yes_no 直接拒绝、nano 转 GUI 编辑器、
 * 需要重新登录或密码输入的命令会被拒绝，避免阻塞图形循环。 */
void shell_set_noninteractive(int on);
int  shell_noninteractive(void);

#endif
