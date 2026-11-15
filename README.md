# SaylterOS

SaylterOS 是一个运行在 x86（32 位）上的裸金属操作系统：没有宿主系统、没有标准库、没有现成文件系统，从 MBR 开始全部自己实现。引导链由汇编完成，进入保护模式后交给 C/C++ 内核，提供带用户账户与文件权限的 TTY Shell、自研 SlyzFS 文件系统、全屏文本编辑器，以及 ACPI S5 关机与电源键支持。

当前版本：**v0.0.3-Alpha3**（代码内版本号 `v0.3`）。

---

## 功能概览

| 模块 | 状态 |
|---|---|
| 自研引导链（MBR → Stage2 → 保护模式 → 内核） | 可用 |
| VGA 文本模式 TTY Shell | 可用 |
| VBE 图形界面 | 仅 Logo 展示 |
| SlyzFS 文件系统（目录、文件、权限） | 可用 |
| 用户账户、登录、口令哈希 | 可用 |
| 文件权限（mode / owner，FS 层强制） | 可用 |
| 全屏文本编辑器 `nano` | 可用 |
| ACPI S5 关机 + 电源键识别 | 可用 |
| 8042 硬件重启 | 可用 |
| 动态提示符 + TTY 底部安全区 | 可用 |
| SLYZ 二进制字节码（SLYZBIN v1） | 可用，同时兼容旧文本脚本 |
| 网络、多任务、分页、SMP | 未实现 |

---

## 快速开始

### 依赖

构建脚本针对 Windows + PowerShell，工具路径写在 [build.ps1](build.ps1) 顶部，按自己的环境修改：

| 工具 | 默认路径 | 用途 |
|---|---|---|
| NASM | `D:\msys2-64\mingw64\bin\nasm.exe` | 汇编引导链 |
| gcc (mingw32) | `D:\msys2-64\mingw32\bin\gcc.exe` | 编译 C 内核 |
| g++ (mingw32) | `D:\msys2-64\mingw32\bin\g++.exe` | 编译 SlyzFS |
| objcopy | `D:\msys2-64\mingw32\bin\objcopy.exe` | ELF → 纯二进制 |
| dd | `D:\msys2-64\usr\bin\dd.exe` | 拼接镜像 |
| QEMU | `qemu-system-i386` | 运行 |

### 构建

```powershell
powershell -NoProfile -File build.ps1
```

产物：

| 文件 | 说明 |
|---|---|
| `build/saylter.img` | VBE 版软盘镜像 |
| `build/saylter-tty.img` | TTY 版软盘镜像 |
| `build/saylter-ide.img` | 32 MB IDE 硬盘镜像，SlyzFS 位于 LBA 2048 |

每次构建都会**重新创建** `saylter-ide.img`，其中已写入的用户、文件会丢失。

### 运行

```powershell
powershell -NoProfile -File run-ide.ps1
```

等价命令：

```powershell
qemu-system-i386 -machine pc -cpu pentium2 -m 64 -display gtk `
  -boot c -drive format=raw,file=build\saylter-ide.img,if=ide `
  -snapshot -no-reboot
```

查看串口日志（调试时常用）：

```powershell
qemu-system-i386 -drive format=raw,file=build\saylter-ide.img,if=ide `
  -display none -serial file:build\serial.log `
  -monitor tcp:127.0.0.1:4444,server,nowait -m 128
```

---

## 登录与账户

首次启动时如果检测不到 `etc\passwd`，系统会自动创建账户库并写入一个**无密码的 root**：

```
SaylterOS login: root
Password: (none)
Welcome, root (uid 0)
root@os:~#
```

创建新用户（需要 root）：

```
root@os:~# useradd alice
useradd: created 'alice' with uid 1, home home\alice
useradd: set a password with 'passwd alice'
root@os:~# passwd alice
New password: ******
Retype new password: ******
passwd: password updated for 'alice'
```

账户存储：

- `etc\passwd`（0644）：`name:uid:gid:home`
- `etc\shadow`（0600）：`name:salt:hash`，hash 为 `SHA-256(salt + password)`

root 的 hash 为空表示无密码。普通用户读取 `\etc\shadow` 会被文件系统直接拒绝。

---

## 命令参考

| 命令 | 说明 |
|---|---|
| `help` / `min` | 帮助 / 精简命令列表 |
| `clear` / `cls` | 清屏 |
| `echo <text>` | 输出文本 |
| `dir` / `ls` | 列出当前目录（含 MODE、UID、SIZE） |
| `pwd` | 显示当前目录 |
| `cd <path>` | 切换目录，支持 `..`、`.`、绝对路径 |
| `mkdir` / `rmdir` | 创建 / 删除目录 |
| `cat <file>` | 输出文件内容 |
| `nano <file>` / `edit` | 全屏编辑器（Ctrl+S 保存、Ctrl+X 退出、Ctrl+K 删行） |
| `run <file>` | 执行 SLYZ 镜像或文本脚本 |
| `ask <prompt>` | 读取 y/n 确认 |
| `dd` | 扇区 / 文件 / 文本 / stdout 之间的裸数据复制 |
| `remove <start> <end>` | 清除扇区 |
| `whoami` | 当前用户与 uid |
| `useradd <name>` | 创建用户（root） |
| `passwd [name]` | 修改口令（自己或 root 指定他人） |
| `su [name]` | 切换用户（默认 root） |
| `logout` | 注销并回到登录界面 |
| `chmod <octal> <path>` | 修改权限位 |
| `chown <uid> <path>` | 修改 owner（root） |
| `acpi` | 查看 FADT / PM1 / 电源键状态 |
| `poweroff` / `shutdown` / `halt` | ACPI S5 关机 |
| `reboot` | 通过 8042 复位线重启 |
| `version` / `uname` / `date` / `mem` / `ps` | 系统信息 |
| `gui` | 进入 VBE Logo 界面（仅 VBE 版） |
| `exit` / `quit` | 退出 Shell |

编辑器快捷键：方向键、Home/End、PageUp/PageDown 移动，Backspace/Delete 删除，Tab 插入 4 空格。

---

## 目录结构

```
SaylterOS/
├── build.ps1                    唯一的构建入口（生成三个镜像）
├── run-ide.ps1                  以 IDE 硬盘方式启动 QEMU
├── linker.ld                    内核链接脚本（起始 0xA200）
├── src/
│   ├── boot/
│   │   ├── boot.asm             MBR，分段加载后续镜像
│   │   ├── stage2.asm           Stage2（A20、内存探测）
│   │   ├── pm.asm               进入 32 位保护模式
│   │   └── entry32.asm          32 位入口，清 BSS 后跳入 C 内核
│   ├── kernel/
│   │   ├── kernel.c             kernel_main，初始化串口/ACPI/界面
│   │   ├── shell.c              Shell、登录会话、命令分发、GUI 入口
│   │   ├── tty.c / tty.h        VGA 文本输出、滚动区与安全区
│   │   ├── keyboard.c / .h      PS/2 键盘，扫描码到字符
│   │   ├── mouse.c / .h         鼠标（VBE 版）
│   │   ├── vbe.c / vbe.h        VBE 帧缓冲
│   │   ├── font16x32.c          16x32 点阵字模
│   │   ├── editor.c / .h        nano 风格全屏编辑器
│   │   ├── auth.c / .h          账户库、登录、口令哈希
│   │   ├── sha256.c / .h        SHA-256（FIPS 180-4）
│   │   ├── acpi.c / .h          RSDP/FADT 解析、S5、电源键轮询
│   │   ├── slyzvm.c / .h        SLYZBIN 字节码虚拟机
│   │   ├── reboot.c / .h        8042 硬件复位
│   │   └── io.h                 端口 I/O 内联封装
│   └── fs/
│       ├── slyzfs.cpp / .h      SlyzFS 实现（C++，含权限检查）
│       ├── slyzfs_bridge.cpp    C++ → C 的 extern "C" 桥接
│       └── kernel_disk.cpp      磁盘抽象（ATA PIO 对接）
├── fonts/                       字模源数据与授权说明
├── tools/                       字体生成与调试脚本
└── slyz-vscode-extension/       .slyz 脚本的 VS Code 语法扩展
```

---

## 架构说明

### 引导链与镜像布局

| LBA | 内容 | 加载地址 |
|---|---|---|
| 0 | `boot.bin`（MBR） | `0x7C00` |
| 1–4 | `stage2.bin` | `0x7E00` |
| 5–18 | `pm.bin` | `0x8600` |
| 19+ | `kernel.bin` | `0xA200` |
| 2048+ | SlyzFS 超级块 | — |

`boot.asm` 使用 `int 13h AH=42h`（DAP/LBA）分四段加载，每段都不跨越 64 KB 段边界：

```
LBA 1..63   → 0x7E00
LBA 64..65  → 0xFC00
LBA 66..129 → 0x10000
LBA 130..193→ 0x18000
```

加载上界为 `0x20000`，因此内核的 `.text + .rodata + .data` 必须在此之前结束。

### 内存硬约束

| 约束 | 原因 |
|---|---|
| `.data` 结束 < `0x20000` | 引导只加载到这里，超出部分不会进内存 |
| `.bss` 结束 < `0xA0000` | 640 KB 以上是 VGA 显存与 BIOS ROM 映射区，写入被忽略 |
| 编译必须带 `-mno-sse -mno-sse2 -mno-mmx -mno-3dnow` | 裸内核未置位 `CR4.OSFXSR`，SSE 指令会触发 `#UD` 并卡死 |
| 编译固定 `-O0` | 高优化级别会插入 SSE 指令与栈探针 |

当前实测：`.data` 结束于 `0x1CE44`（余量约 29 KB），`.bss` 结束于 `0x869C0`（距 640 KB 红线约 100 KB）。新增模块后需重新核算，必要时提高 `boot.asm` 的 `IMG_TAIL_SECTORS`（当前 128）。

### SlyzFS

| 项 | 值 |
|---|---|
| 起始 LBA | 2048 |
| 超级块 | 1 扇区（结构 506 字节，读写用 512 字节缓冲中转） |
| FAT | 64 扇区，16384 个 16 位条目，链式分配 |
| 根目录 | 256 扇区，每扇区 8 个条目 |
| 簇大小 | 4 扇区 = 2048 字节 |
| 目录条目 | 71 字节（pack(1)） |
| 数据区起始 | LBA 2369 |
| 32 MB 盘容量 | 15791 个簇 |

目录条目布局：

```
type(1) name(52) start_cluster(2) size(4) created(4) modified(4) mode(2) uid(1) gid(1)
```

`mode` 为 0 表示旧版本创建的条目，按文件 `0600` / 目录 `0700`、owner=root 保守处理。

权限强制点全部在文件系统层：

- `open("r")` 需要读位
- `open("w")` 需要文件写位 + 父目录写/执行位
- `list_dir` 需要目录读位
- `mkdir` / `rmdir` / `remove` 需要父目录写/执行位
- `chmod` 需要 root 或父目录 owner；`chown` 仅 root

root（uid 0）绕过全部检查。

### SLYZ 二进制可执行

`.slyz` 源码由 `tools/windows-slyz-tools` 里的 `slyzc` 编译成 SLYZBIN v1 镜像：

```bat
slyztool compile demo.slyz -o demo.bin
slyztool dump demo.bin
```

镜像结构：32 字节头（魔数 `SLYZBIN\0`、版本、常量池/字节码长度、入口、FNV-1a 校验和）+ 常量池 + 字节码。全部小端、位置无关，Windows 上编译后可直接放进 SlyzFS 由内核 [slyzvm.c](src/kernel/slyzvm.c) 执行；`run` 先检查魔数，不是镜像时回退到文本解析。

把镜像导入系统（`dd` 支持 `file:` 规格）：

```text
root@os:~# dd if=sector:16384 of=file:demo.bin
root@os:~# run demo.bin
```

### 用户系统

- uid 为 8 位（0 = root，1..255 为普通用户）
- 口令只存 `SHA-256(salt + password)`，salt 每次改密重新生成
- `etc\shadow` 权限 0600，普通用户即使知道路径也无法读取
- 账户库的读写都在临时 root 身份下进行（内核内的特权操作）

### ACPI

只解析固定表结构，不解析任何 AML：

```
RSDP（EBDA 或 0xE0000..0xFFFFF）→ RSDT/XSDT → FADT → PM1 寄存器块
```

- S5 关机：向 `PM1_CNT` 写 `SLP_TYP | SLP_EN`
- SLP_TYP 默认 5，失败自动回退 0（QEMU/部分 UEFI 用 0，传统 BIOS 多用 5），也可 `poweroff <0..7>` 手动指定
- 电源键：轮询 `PM1_STS.PWRBTN_STS`，在 Shell、编辑器、GUI 主循环中都会响应
- 不注册 SCI 中断（IRQ 路由需要 AML 的 `_PRT`），因此电源键响应依赖轮询周期

### TTY 安全区

Shell 提示符始终绘制在屏幕底部的保留区，命令输出只滚动上方区域。回车时由 `tty_commit_reserved()` 把输入行整体上提到滚动区，保证历史 prompt 可见、输入行被清空。保留行数按提示符实际宽度动态计算。

---

## 已知限制

- 单核、单线程、无分页、无抢占，所有等待都是忙轮询
- 无 `rm`、`sudo`、用户组、setuid
- 口令哈希无迭代 KDF（无 PBKDF2/Argon2），仅单轮 SHA-256
- VBE 模式只显示 Logo，尚未实现图形 Shell
- SLYZ 虚拟机只支持 `echo/ask/set/if/else/endif/run/clear/help/exit` 与整行 shell 命令
- 每次运行 `build.ps1` 都会重建 `saylter-ide.img`，其中的用户与文件会被清空

---

## 版本历史

| 版本 | 主要内容 |
|---|---|
| v0.0.3-Alpha3 | SlyzFS 接入 Shell、用户账户与权限、nano 编辑器、ACPI S5 与电源键、动态提示符与安全区 |
| v0.0.3-Alpha2 | VBE 图形界面、字体渲染、鼠标支持 |
| v0.0.3-Alpha1 | 保护模式、中断、C 内核与基础 Shell |

---

## 许可

字模数据与生成脚本的授权说明见 [fonts/LICENSE](fonts/LICENSE)。
