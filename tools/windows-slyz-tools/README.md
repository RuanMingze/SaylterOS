# SLYZ Tools for Windows（已废弃）

> 这里的 `slyz_tool.c` 生成 **SLYZBIN v1**，内核 v2 虚拟机不再接受。
> 现行工具链在 `tools/slyzc/`，语言参考见 `docs/slyz-reference.md`。
> 保留本目录仅用于查看历史格式，不要再用它产出系统可执行文件。

把 `.slyz` 源文件编译成 SaylterOS 内核可直接执行的 **SLYZBIN v1** 字节码镜像。

产物不再是批处理或文本脚本，而是一个真正的位置无关二进制：32 字节头 + 常量池 + 字节码，全部小端，没有重定位信息，Windows 上编译完可以直接放进 SlyzFS 交给内核执行。

## 编译工具

```bat
gcc -std=c11 -O2 -Wall -Wextra -o slyztool.exe slyz_tool.c
```

## 用法

```bat
slyztool compile demo.slyz -o demo.bin   编译成二进制镜像
slyztool check demo.slyz                 只做语法检查
slyztool dump demo.bin                   反汇编，查看字节码
slyztool run demo.slyz                   在 Windows 上模拟执行，验证逻辑
slyztool doc                             显示语法参考
```

`translate` 已移除：二进制镜像无法再翻译成 `.bat`，Windows 端改为直接解释同一份字节码来验证编译结果。

## 语法

```slyz
# 注释
echo Hello from SLYZ
ask Continue?
if yes
  echo continue path
else
  echo stop path
endif
```

支持的语句：

| 语句 | 作用 |
|---|---|
| `echo <text>` | 输出一行 |
| `ask <prompt>` | 询问 Y/N，结果写入 last |
| `set yes` / `set no` | 直接设置 last |
| `alloc <bytes>` | 向系统申请内存（1..65535） |
| `free` | 释放最近一次 alloc |
| `if yes` / `if no` | 条件分支，支持 `else` |
| `endif` | 结束条件块 |
| `run <script>` | 调用另一个脚本 |
| `clear` / `cls` | 清屏 |
| `help` | 系统帮助 |
| `exit` / `quit` | 立即结束 |
| 其他命令 | 整行交给 shell 执行 |

## 二进制格式

```
offset 0   magic[8]        "SLYZBIN\0"
offset 8   u16 version     1
offset 10  u16 flags       bit0 = 需要交互
offset 12  u32 const_size  常量池字节数
offset 16  u32 code_size   字节码字节数
offset 20  u32 entry       入口偏移
offset 24  u32 checksum    FNV-1a(const_pool || code)
offset 28  u32 reserved
offset 32  const_pool      '\0' 分隔的字符串
之后       code            字节码
```

指令集：`END/ECHO/ASK/JMP/JZ/JNZ/RUN/EXIT/HELP/CLEAR/SET/NOP`，操作数为 2 字节小端绝对偏移。内核侧实现见 `src/kernel/slyzvm.c`。

单文件上限：常量池与字节码各 4 KB。

## 把镜像放进 SaylterOS

系统里没有 U 盘挂载，用 `dd` 把扇区内容导出成文件：

```text
root@os:~# dd if=sector:16384 of=file:demo.bin
dd: 512 bytes (1 block(s) x 512) done
root@os:~# run demo.bin
Hello from SLYZ on Windows
Continue? [Y/N] y
continue path
```

宿主机写入扇区（构建脚本同款工具）：

```powershell
dd.exe if=demo.bin of=build\saylter-ide.img bs=512 seek=16384 conv=notrunc,fsync
```

`seek=16384` 只是示例位置，确保目标扇区在 SlyzFS 里没有被分配。
