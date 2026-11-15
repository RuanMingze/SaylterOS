# SLYZ Tools for Windows

这是一个 Windows 上的 C 语言 SLYZ 工具集，提供三大能力：

1. 语法检查
2. 翻译成 .bat 来运行
3. 打开 SLYZ 开发文档

## 用法

```bat
slyztool check demo.slyz
slyztool translate demo.slyz demo.bat
slyztool run demo.slyz
slyztool doc
```

## 生成可执行文件

```bat
gcc.exe -std=c11 -O2 -Wall -Wextra -o slyztool.exe slyz_tool.c
```

## 语法特性

- `echo` 输出文本
- `ask` 询问 `Y/N`
- `if yes` / `if no`
- `endif` 结束分支
- `dir` / `ls`
- `run` 调用另一个脚本
- `exit` / `quit`
- `help`

## 文档

详细文档见：

- docs/slyz-reference.md


## 最佳使用方式

在 Windows 上，最稳妥的方式是：

- 先用 `check` 检查语法
- 再用 `translate` 生成 `.bat`
- 最后用 `run` 执行，或者直接双击 `.bat`
