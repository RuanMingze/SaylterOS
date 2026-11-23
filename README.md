# SaylterOS

SaylterOS 是一个运行在 x86_64（64 位长模式）上的裸金属操作系统：没有宿主系统、没有标准库、没有现成文件系统，从 MBR 开始全部自己实现。引导链由汇编完成，先进入 32 位保护模式完成内核搬运与页表建立，再切入 64 位长模式交给 C/C++ 内核，提供完整图形桌面、SMP 多核、4KB 分页与 swap、自研 SlyzFS 文件系统、E1000 网络栈以及 ACPI S5 关机与电源键支持。

SaylterOS v0.3 Beta2 已推出！此版本带来了 SlyzFS v2、NVMe 支持，播放音乐支持等多项功能，建议升级。我们预计会在下一个版本推出 SaylterOS v0.3 RC 版本更新。
当前版本：**v0.3 Beta2**（代码内版本号 `v0.3 Beta2`）。
⚠️警告：此版本未进行实机验证，并且 SlyzFS 的代码里存在危险行为，会在挂载失败时自动格式化目标分区。谨慎实机运行！我不为冒着风险尝试实体机运行导致丢数据承担责任。

---

## 功能概览

| 模块 | 状态 |
|---|---|
| 自研引导链（MBR → Stage2 → 保护模式 → 长模式 → 内核） | 可用 |
| VGA 文本模式 TTY Shell | 可用 |
| 完整图形桌面（窗口管理、任务栏、文件管理器、编辑器、终端、浏览器、设置、任务管理器、计算器） | 可用 |
| 桌面右键菜单、桌面图标、鼠标滚轮与终端历史滚动条 | 可用 |
| SlyzFS 文件系统（目录、文件、权限） | 可用 |
| 用户账户、登录、口令哈希 | 可用 |
| 文件权限（mode / owner，FS 层强制） | 可用 |
| 受保护文件（MODE_PROTECTED，root 也不能删除） | 可用 |
| 全屏文本编辑器 `nano` | 可用 |
| 内核堆（6 MB，first-fit + 相邻合并） | 可用 |
| 任务与内存配额、OOM 回收 | 可用（协作式调度） |
| 4KB 分页、按需分页、缺页处理 | 可用 |
| swap 文件分页（etc\pagefile.os，16 MB 容量） | 可用 |
| SMP 多核启动与并行工作池（0xF0 IPI + 静态轮转分片） | 可用 |
| 内核崩溃蓝屏（冻结硬件） | 可用，致命缺页会进入蓝屏 |
| ACPI S5 关机 + 电源键识别 | 可用 |
| 8042 硬件重启 | 可用 |
| 动态提示符 + TTY 底部安全区 | 可用 |
| SLYZ 二进制字节码（SLYZBIN v1） | 可用，同时兼容旧文本脚本 |
| E1000 有线网络（ARP/IPv4/ICMP/UDP/DNS/TCP/HTTP） | 可用 |

---

## 快速开始

### 依赖

构建脚本针对 Windows + PowerShell，工具路径写在 [build.ps1](build.ps1) 顶部（我这边的 msys2 目录是“msys2-64”，是我这边的环境遗留问题，实际应该是“msys64”，请务必自行改成正确的路径），按自己的环境修改：

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
| `build/saylter-ide.img` | 32 MB IDE 硬盘镜像，SlyzFS 位于 LBA 8192 |

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
| `bg <file>` | 在后台任务中运行 SLYZ 镜像 |
| `app <file.slys>` | 加载 `.slys` 模块（可执行 musl 静态应用）并以退出码返回 |
| `kill <pid>` | 终止任务；`kill 0` 触发内核蓝屏 |
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
| `mem` | 内核堆统计（总量/已用/峰值/失败次数） |
| `ps` | 任务列表（配额、已用、峰值、步数） |
| `gui` | 进入 VBE Logo 界面（仅 VBE 版） |
| `exit` / `quit` | 退出 Shell |

编辑器快捷键：方向键、Home/End、PageUp/PageDown 移动，Backspace/Delete 删除，Tab 插入 4 空格。

`app` 加载的 `.slys` 既可以是驱动，也可以是 musl 静态链接的用户程序
（`SlysKernelApi` 版本 2 追加了 write/read/open/close/lseek/fstat/
time_ms/map/unmap 九个 POSIX 字段）。musl 源码在 `third_party/musl`，
适配层在 `src/slyzlibc`，用 `tools/build-musl.ps1` 构建。
内核用 MinGW（Win64 ABI）编译、musl 模块用 x86_64-elf（SysV ABI）编译，
跨 ABI 调用统一由 `src/slyzlibc/msabi.s` 的 thunk 完成，模块侧不需要
感知参数寄存器差异。

---

## 目录结构

```
SaylterOS/
├── build.ps1                    唯一的构建入口（生成 IDE 与安装盘镜像）
├── run-ide.ps1                  以 IDE 硬盘方式启动 QEMU（-Smp / -Persist / -Headless）
├── linker.ld                    内核链接脚本（起始 0x100000）
├── src/
│   ├── boot/
│   │   ├── boot.asm             MBR，分段加载后续镜像
│   │   ├── stage2.asm           Stage2（A20、内存探测、VBE 模式）
│   │   ├── pm.asm               保护模式 → 页表 → 长模式 → IDT/PIC
│   │   ├── entry64.asm          64 位入口，清 BSS 后跳入 kernel_main
│   │   └── ap_tramp.asm         AP 启动 trampoline（16→32→64 位）
│   ├── kernel/
│   │   ├── kernel.c             kernel_main，初始化串口/显示/SMP
│   │   ├── gui.c                图形桌面：启动页、登录、窗口、7 个应用
│   │   ├── vm.c / vm.h          4KB 页帧、按需分页、时钟换出、swap
│   │   ├── smp.c / smp.h        多核启动、0xF0 IPI 并行工作池
│   │   ├── apic.c / apic.h      Local APIC 驱动
│   │   ├── net.c / net.h        E1000 + TCP/IP 栈
│   │   ├── netcmds.c            ip / curl / ping / nslookup 命令
│   │   ├── shell.c              Shell、登录会话、命令分发、GUI 入口
│   │   ├── tty.c / tty.h        字符终端、滚动区与安全区
│   │   ├── keyboard.c / .h      PS/2 键盘，扫描码到字符
│   │   ├── mouse.c / .h         鼠标（含 IntelliMouse 滚轮）
│   │   ├── vbe.c / vbe.h        VBE 帧缓冲与裁剪
│   │   ├── ui.c / ui.h          图形组件库
│   │   ├── html.c / html.h      GUI 浏览器的 HTML/CSS 子集渲染
│   │   ├── font16x32.c          16x32 点阵字模
│   │   ├── editor.c / .h        nano 风格全屏编辑器
│   │   ├── auth.c / .h          账户库、登录、口令哈希
│   │   ├── sha256.c / .h        SHA-256（FIPS 180-4）
│   │   ├── acpi.c / .h          RSDP/FADT 解析、S5、电源键
│   │   ├── acpica_osl.c         ACPICA 平台层（AML 解释器）
│   │   ├── slyzvm.c / .h        SLYZBIN 字节码虚拟机
│   │   ├── task.c / mem.c       协作式任务、内存配额、内核堆
│   │   ├── bsod.c / .h          蓝屏：冻结中断与任务
│   │   ├── system_cfg.c         启动模式 / 时区配置
│   │   └── io.h                 端口 I/O 内联封装
│   └── fs/
│       ├── slyzfs.cpp / .h      SlyzFS 实现（C++，含权限与受保护位）
│       ├── slyzfs_bridge.cpp    C++ → C 的 extern "C" 桥接
│       └── kernel_disk.cpp      磁盘抽象（ATA PIO 对接）
├── third_party/acpica/          AML 解释器
├── fonts/                       字模源数据与授权说明
├── tools/                       字体生成与调试脚本
├── samples/                     SLYZ / GUI 示例程序
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
| 19+ | `kernel.bin` | `0x100000` |
| 8192+ | SlyzFS 超级块 | — |

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
| `.data/.reloc` 结束 < `0x3F0000` | `0x3FF000` 是内核初始栈顶，镜像不能顶到栈 |
| `.bss` 结束 < `0x1400000` | BSS 从 18 MB 起，必须留在 20 MB 内存范围内 |
| 编译必须带 `-mno-sse -mno-sse2 -mno-mmx -mno-3dnow` | 裸内核未置位 `CR4.OSFXSR`，SSE 指令会触发 `#UD` 并卡死 |
| 编译默认 `-O0`，绘制热路径 `-O2` | 高优化级别会插入 SSE 指令与栈探针，热路径显式关闭循环改写 |

当前实测：内核镜像约 1.29 MB，装载结束于 `0x249200`；`.bss` 结束于 `0x1358E80`（距 20 MB 上界约 690 KB）。新增模块后 `build.ps1` 会自动重新核算并拒绝越界构建。

> SlyzFS 起始 LBA 必须大于内核装载上界。文件系统曾从 LBA 2048（1 MB）
> 开始，内核超过 1 MB 后 `mkfs` 的超级块写入会覆盖内核尾部
> `.rodata/.data`，表现为启动时 #GP；现已移动到 LBA 8192（4 MB）。

### SlyzFS

| 项 | 值 |
|---|---|
| 起始 LBA | 8192 |
| 超级块 | 1 扇区（结构 506 字节，读写用 512 字节缓冲中转） |
| FAT | 64 扇区，16384 个 16 位条目，链式分配 |
| 根目录 | 256 扇区，每扇区 8 个条目 |
| 簇大小 | 4 扇区 = 2048 字节 |
| 目录条目 | 71 字节（pack(1)） |
| 数据区起始 | LBA 8513 |
| 32 MB 盘容量 | 14255 个簇 |

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

脚本可以申请内存：

```slyz
alloc 4096
echo got 4KB
free
```

`alloc` 走当前任务的内存配额，超出配额时该任务会被 OOM 回收（见「内存管理与任务」）。

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

### 内存管理与任务

内核堆固定在物理地址 `0x400000`（4 MB 起 6 MB），不占用 `.bss`，分配器为 first-fit + 相邻空闲块合并，带总/已用/峰值/失败次数统计（`mem` 命令）。4 KB 页帧池从 24 MB 起，上界为检测到的 RAM − 4 MB。

任务模型（`task.c`）：

| 能力 | 实现 |
|---|---|
| 独立内核栈 | 静态池，每任务 8 KB，共 4 个槽位 |
| 状态机 | FREE / READY / RUNNING / DEAD |
| 内存配额 | 默认 64 KB，`task_alloc` 记账，超出即失败 |
| OOM 回收 | 配额超限时打印诊断、标记任务 DEAD、释放其全部内存 |
| 调度 | 协作式：VM 每 1024 条指令、shell 空闲时 `task_yield()` |
| 上下文切换 | 保存 callee-saved 寄存器与 esp，切换点固定在 yield/exit，不在中断里切栈 |

`run` 一个 SLYZ 镜像时，脚本在独立任务中执行，shell 通过 `task_wait` 等待；脚本申请内存超过配额会被杀掉并回收，shell 继续运行。内核任务（id 0）没有配额限制。

`bg` 把脚本放进后台任务后立即返回 shell，任务在 shell 空闲时被调度；`wait` 指令会让任务停在原地反复让出 CPU，直到被 `kill <pid>` 终止——被杀后调度器不再选择它，栈永久冻结。

`kill 0` 不会真的终止内核：它会要求二次确认，然后进入崩溃蓝屏（[bsod.c](src/kernel/bsod.c)）。蓝屏不是装饰——关闭中断、屏蔽 8259 全部 IRQ、停止所有任务、不再执行任何磁盘写入，CPU 冻结在 `cli; hlt` 循环里，只能靠电源键或复位离开。

**任务配额与分页是两个层次**：配额是软件层面的资源上限，用来演示 OOM 语义；硬件分页已经实现，但当前只有一个 16 MB 的按需窗口（`0x20000000`），没有按进程的独立地址空间。窗口外或保护违规的缺页会进入蓝屏，而不是杀死某个进程。跨核 TLB shootdown 尚未实现，AP 不访问按需窗口。

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

- SMP 只在 3 颗 vCPU 的 QEMU 环境验证；`-smp 4` 时第 4 颗核在 QEMU TCG 下启动超时，工作池只使用成功在线的核
- 分页只有一个 16 MB 按需窗口，没有按进程地址空间隔离；致命缺页直接进入蓝屏，没有进程级 OOM-kill
- 跨核 TLB shootdown 尚未实现，AP 不访问按需分页区；GUI 并行渲染只用于大面积全屏重绘
- 无 `sudo`、用户组、setuid
- 口令哈希无迭代 KDF（无 PBKDF2/Argon2），仅单轮 SHA-256
- 网络为同步轮询实现，不支持中断收包与 DHCP；IP 为静态配置
- SLYZ 虚拟机只支持 `echo/ask/set/if/else/endif/run/clear/help/exit` 与整行 shell 命令
- 每次运行 `build.ps1` 都会重建 `saylter-ide.img`，其中的用户与文件会被清空

---

## 版本历史

| 版本 | 主要内容 |
|---|---|
| v0.3 Beta1 | 完整 GUI（窗口管理、任务管理器、计算器、鼠标滚轮与终端滚动条、桌面右键）、SMP 多核并行工作池、4KB 分页与 swap 文件、E1000 网络栈、ACPI 电源管理、致命缺页接入蓝屏 |
| v0.0.3-Alpha3 | SlyzFS 接入 Shell、用户账户与权限、nano 编辑器、ACPI S5 与电源键、动态提示符与安全区 |
| v0.0.3-Alpha2 | VBE 图形界面、字体渲染、鼠标支持 |
| v0.0.3-Alpha1 | 保护模式、中断、C 内核与基础 Shell |

---

## 许可

字模数据与生成脚本的授权说明见 [fonts/LICENSE](fonts/LICENSE)。
