# SLYZ 参考文档

SLYZ 现在有两种形态：

- **源码** `.slyz`：人写的文本脚本
- **镜像** `.bin`：`slyzc` 编译出的 SLYZBIN v1 字节码，内核直接执行

内核仍然兼容旧文本脚本（`run xxx.slyz` 会走文本解析），但正式分发应使用二进制镜像：启动更快、语法错误在编译期就被拦住、也不需要运行时逐行解析。

## 语法

### echo

```slyz
echo Hello, World!
```

输出一行文本。

### ask

```slyz
ask Are you ready?
```

显示提示并等待 `Y/N`，结果写入内部变量 `last`：`Y` 为 1，`N` 为 0。

### set

```slyz
set yes
set no
```

不经过交互直接设置 `last`，便于构造顺序逻辑。

### if / else / endif

```slyz
if yes
  echo yes branch
else
  echo no branch
endif
```

`if yes` 在 `last == 0` 时跳过整个分支；`if no` 在 `last != 0` 时跳过。`else` 可选。

### run

```slyz
run demo.bin
```

调用另一个脚本。内核支持两层嵌套，超过会报 `script nesting too deep`。

### 其他命令

```slyz
dir
pwd
clear
help
```

不以 `echo/ask/set/if/else/endif/run/exit/help/clear` 开头的整行会作为 shell 命令执行。

### exit

```slyz
exit
```

立即结束脚本。

## 编译

```bat
slyztool compile demo.slyz -o demo.bin
slyztool dump demo.bin
```

反汇编示例：

```
SLYZBIN v1  const=61 code=19 entry=0 checksum=0x2DDA670F
0000  ECHO  "Hello from SLYZ on Windows"
0003  ASK   "Continue?"
0006  JZ    0012
0009  ECHO  "continue path"
0012  JNZ   0018
0015  ECHO  "stop path"
0018  END
```

`ASK` 之后的 `JZ` 对应 `if yes`，`JNZ` 对应 `if no`。

## 语法检查规则

- `if` 后必须是 `yes` 或 `no`（也接受 `1` / `0`）
- `else` 必须有对应的 `if`
- 每个 `if` 必须有 `endif`
- `ask` / `echo` 必须有参数
- 常量池或字节码超过 4 KB 会报错

## 字节码格式

见 [README.md](../README.md) 的「二进制格式」一节，以及内核实现 `src/kernel/slyzvm.c`。

## 设计目标

- 一个可在 Windows 上离线编译、在裸机系统里直接执行的脚本格式
- 位置无关、无重定位、可校验（FNV-1a）
- 编译期拦住语法错误，运行时只做边界检查
