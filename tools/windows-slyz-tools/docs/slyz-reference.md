# SLYZ 参考文档

## 语法概览

SLYZ 是一个极简脚本语言，目标是让裸机脚本像批处理一样执行。当前 Windows 工具会把它翻译成 .bat，然后交给 cmd.exe 执行。

## 关键语法

### 1. echo

```slyz
echo Hello, World!
```

输出文字。

### 2. ask

```slyz
ask Are you ready?
```

显示提示并等待用户输入 `Y/N`，内部简化为 `SLYZ_LAST`，`Y` 记为 `1`，`N` 记为 `0`。

### 3. if yes / if no

```slyz
if yes
  echo yes branch
endif

if no
  echo no branch
endif
```

条件分支：只有当前 `SLYZ_LAST` 与条件一致时才执行分支内容。

### 4. endif

结束 `if` 分支。

### 5. dir / ls

```slyz
dir
```

列出当前目录。

### 6. run

```slyz
run demo.slyz
```

调用另一个 SLYZ 文件。Windows 版会自动翻译并递归调用批处理。

### 7. exit / quit

```slyz
exit
```

立即退出脚本。

### 8. help

```slyz
help
```

显示帮助信息。

## 示例脚本

```slyz
echo Hello from SLYZ
ask Continue?
if yes
  echo continue path
endif
if no
  echo stop path
endif
```

## Windows 工具

运行方式：

```bat
slyztool check demo.slyz
slyztool translate demo.slyz demo.bat
slyztool run demo.slyz
slyztool doc
```

## 语法检查规则

- `if` 后必须是 `yes` 或 `no`
- 缺少 `endif` 会报错误
- `ask` 必须有提示文本
- `echo`/`run` 也要求参数
- 未知命令会被判为错误

## 设计目标

- 在 Windows 上做最接近裸机 SLYZ 行为的脚本体验
- 先支持简单分支和交互
- 通过 `translate` 方案让脚本能直接在 cmd.exe 中运行
