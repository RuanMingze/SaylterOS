# VizCormin 代码编辑器：架构与链接规范

> 目标：对标 VSCode 的编辑体验，去掉插件系统与 Copilot，
> 在 SaylterOS 裸机环境下运行。当前阶段只落地**模块链接**与
> **架构基线**，不改动已恢复正常的桌面运行路径。

---

## 1. 现实约束与结论

1. Monaco 是 TypeScript/JavaScript，依赖 DOM、`window`、`requestAnimationFrame`
   和浏览器布局引擎。裸机内核没有 JS 运行时，直接“移植 Monaco 源码”
   不可行。
2. 因此采用**概念移植**：保留 Monaco 的模型与交互设计（文本模型、
   视图模型、命令/快捷键、查找替换、多光标、折叠、diff），
   把渲染后端替换为 SlyzUI 画布（backbuffer + 脏矩形 + 字形图集）。
3. Oniguruma 是 TextMate 语法的基础。两条路线：
   - 近期：把 Monaco Monarch 语法（JSON 描述）编译成 Slyz 词法器；
   - 中期：移植 Oniguruma 的 C 子集，作为独立 `.slyzd` 模块。
4. 语言服务先做**进程内**词法/轻量语法分析；协议层按 LSP 风格设计，
   为将来独立语言服务进程留边界，但不实现扩展宿主。

---

## 2. 模块链接模型

### 2.1 固定基址

所有 Slyz 模块在加载时使用**固定物理地址**（内核 4GB 恒等映射），
不做 ASLR，也不实现 MMU 隔离：

| 模块 | 加载地址 |
|---|---|
| audio.slys | `0x02000000` |
| slyzfs2.slys | `0x04000000` |
| vizcormin.slyzd | `0x06000000` |
| oniguruma.slyzd | `0x0A000000` |
| 其他应用 | 由模块头声明，加载器校验不重叠 |

固定基址让“动态链接”退化为**加载期符号解析 + 绝对地址回填**，
不需要 PIC，也不需要运行时重定位器。代价是模块不能随意搬移，
这在本项目“只操作启动盘、单一地址空间”的约束下是可接受的。

### 2.2 两种产物

- `.slyzt`：静态链接。构建期解析并固定符号，运行期不做名字查找。
  **单文件大包方案已否决**（用户明确：单文件本身不稳定），
  具体形态待确认：
  - 方案 A：每个应用一个静态可执行文件，依赖代码只并入该应用；
  - 方案 B：应用与库保持独立文件，构建期生成固定绑定清单，
    运行期按清单直接加载，不搜索、不解析名字。
- `.slyzd`：动态链接。镜像带导出表、导入表、重定位表与依赖列表；
  加载器解析依赖闭包并回填导入槽。适合 VizCormin、Oniguruma、
  共享 UI/运行时。

两者共用同一套 104 字节 packed 头布局，用 magic 区分：
`SLYZT` / `SLYZD`。现有 `.slys` 继续作为“固定地址裸机模块”保留，
`.slyzt` 是它的严格超集（多了符号表），`.slyzd` 再加导入/重定位。

### 2.3 磁盘结构（小端）

```c
struct SlyzModuleHeader {
    uint8_t  magic[8];        /* "SLYZT\0\0\0" 或 "SLYZD\0\0\0" */
    uint16_t version;         /* 格式版本，当前 1 */
    uint16_t flags;           /* bit0 = 动态链接，bit1 = 允许共享 */
    uint32_t header_size;     /* 104（packed） */
    uint32_t load_addr;
    uint32_t image_size;
    uint32_t bss_size;
    uint32_t entry_offset;    /* 相对 load_addr */
    uint32_t image_off;       /* 镜像在文件中的偏移 */
    uint32_t export_off;      /* 导出表偏移，0 = 无 */
    uint32_t export_count;
    uint32_t import_off;      /* 导入表偏移 */
    uint32_t import_count;
    uint32_t reloc_off;       /* 重定位表偏移 */
    uint32_t reloc_count;
    uint32_t dep_off;         /* 依赖表偏移 */
    uint32_t dep_count;
    uint32_t checksum;        /* FNV-1a 覆盖 image */
    char     name[32];        /* UTF-8 模块名，不含扩展名 */
};
```

```c
struct SlyzSymbol {
    char     name[64];        /* 导出符号，C 风格，无修饰 */
    uint32_t offset;          /* 相对 load_addr */
    uint16_t kind;            /* 0 函数，1 数据，2 只读数据 */
    uint16_t version;         /* 主版本，导入必须相等 */
    uint32_t reserved;
};

struct SlyzImport {
    char     name[64];
    uint32_t slot_offset;     /* 模块内 8 字节地址槽，相对 load_addr */
    uint16_t kind;
    uint16_t version;
    uint32_t reserved;
};

struct SlyzReloc {
    uint32_t offset;          /* 需要回填的位置，相对 load_addr */
    uint8_t  type;            /* 1 = ABS64，2 = ABS32 */
    uint8_t  reserved[3];
};

struct SlyzDep {
    char     name[32];
    uint16_t min_version;
    uint16_t flags;
};
```

### 2.4 加载算法

1. 读取头，校验 magic、`header_size`、`image_size <= 4MB`、
   `bss_size <= 64MB`、FNV-1a 校验和。
2. 递归解析依赖表：
   - 按模块名在“已加载表”中查找；命中则复用；
   - 未命中则从启动盘 `system\modules\<name>.slyzd` 读取；
   - 检测循环依赖与地址区间重叠；
   - 依赖的主版本必须满足 `min_version`。
3. 复制镜像到 `load_addr`，清零 BSS。
4. 对每个导入项，在依赖闭包的导出表中按 `(name, version, kind)` 查找；
   找到后把 `load_addr + offset` 写入 `load_addr + slot_offset`。
5. 应用重定位：`ABS64` 写 `load_addr + addend`，`ABS32` 写低 32 位。
6. 把导出表注册到全局符号表（供后续模块加载）。
7. 调用入口：`fn(const SlyzKernelApi *api)`，返回值是模块导出表或
   驱动 ABI 指针；由模块类型决定。
8. 卸载：引用计数归零且没有外部导出引用时才释放；当前阶段
   只加载不卸载，先保证生命周期简单可控。

### 2.5 构建流程

```
slyz3c compile foo.slyz  --baremetal --dll --org 0x06000000 \
        --export format_doc --export open_file -o foo.asm
nasm -f bin foo.asm -o foo.bin
mkslyzd foo.bin foo.slyzd --name vizcormin \
        --dep oniguruma@1 --dep visui@1 \
        --export-file foo.exports --import-file foo.imports
```

- `slyz3c --dll` 为每个外部调用生成“导入槽加载”指令，
  并把槽位登记到 `.imports`；
- `mkslyzd` 生成三张表并写头；
- 静态链接器（`slyztlink` 暂缓）：按确认后的方案 A/B 实现，
  不再采用“所有模块打成一个全局大包”的形态。

### 2.6 当前阶段要交付的最小闭环

1. `src/include/slyzlink.h`：上述结构体与常量（本次已落地）。
2. `tools/mkslyzd`：按规范生成 `.slyzd`。
3. `slys.c` 增加 `slys_load_dynamic()`：依赖解析、符号回填、重定位。
4. 一个测试模块对：`libmath.slyzd` 导出 `add`，`demo.slyzd` 导入并调用；
   串口打印结果。
5. 现有 `.slys` 路径保持不变，音频与文件系统不受影响。

---

## 3. VizCormin 架构

### 3.0 安装位置

VizCormin 是**预装应用**，不是系统组件，不进入 `C:\system`：

```
C:\Programs\VizCormin\
    vizcormin.slyzd          编辑器外壳
    viseditor.slyzd          文本模型/视图/命令
    visui.slyzd              画布渲染
    oniguruma.slyzd          正则引擎（后续）
    grammars\                Monarch/TextMate 语法
    themes\                  主题
```

公共运行时（内存、文件、UI 基础）放在 `C:\system\modules`，
由系统维护；IDE 只依赖它们的稳定 ABI。这样升级/卸载 IDE
不会改动系统文件，系统更新也不会覆盖 IDE。

### 3.1 模块划分

| 模块 | 职责 |
|---|---|
| `vizcormin.slyzd` | 编辑器外壳：文件树、标签页、命令面板、快捷键、状态栏 |
| `viseditor.slyzd` | 文本模型、视图模型、撤销栈、多光标、查找替换、折叠 |
| `visui.slyzd` | 画布渲染：行缓存、字形图集、脏矩形、滚动、minimap |
| `oniguruma.slyzd` | 正则引擎（后续），TextMate 语法支持 |
| `vizlang.slyzd` | 内置语言词法/语法、括号匹配、自动缩进 |

### 3.2 文本模型

- **Piece Table**（只追加的原始缓冲 + 片段链表）：
  - 插入/删除 O(log n) 定位、O(1) 追加；
  - 支持超大文件（>100MB）按需分页，不整份进内存；
  - 与 SlyzFS 的按需读取结合，打开文件只加载索引与可视区。
- 行索引：每 1024 行一个锚点，按行号二分定位。
- 撤销栈：按编辑事务分组，保留选择区与光标，合并连续输入。

### 3.3 渲染

- 只重绘脏行；每个可视行缓存“字形 ID + 颜色 + x 偏移”的显示列表；
- 字形图集：ASCII 与常用 CJK 预烘焙，异体字按需光栅化；
- 光标/选择/装饰单独一层，避免改动文本层；
- 目标：1600x900 下单帧 < 8ms（真机），TCG 下 < 33ms。

### 3.4 功能清单（对标 VSCode，去掉插件/Copilot）

必须：
- 多标签、文件树、面包屑、状态栏；
- 多光标、列选择、选择所有出现、跳转括号；
- 撤销/重做、查找/替换（含正则）、全局搜索；
- 语法高亮（Monarch 语法编译）、括号匹配、自动缩进；
- 代码折叠、minimap、split view、diff 编辑器；
- 命令面板、可配置快捷键、主题、设置；
- 中文输入法、超大文件模式、只读保护文件。

明确不做：
- 扩展宿主、插件市场、Copilot/聊天、远程开发、调试适配器、
  Notebook、任务系统。

### 3.5 Oniguruma 路线

1. 先把 Monaco Monarch 的 JSON 语法编译成 Slyz 词法器（覆盖
   C/C++、Rust、JS/TS、Python、Slyz、JSON、Markdown）。
2. 再移植 Oniguruma 的正则子集：字符类、量词、分组、反向引用、
   前后瞻、UTF-8；不移植编码转换与多编码 API。
3. TextMate grammar 在构建期转换为内部规则表，避免运行时解析
   plist/JSON 的性能与内存开销。

---

## 4. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| M0 | 链接规范、`mkslyzd`、动态加载器 | `demo.slyzd` 调用 `libmath.slyzd` 成功 |
| M1 | Piece Table、撤销、光标、打开/保存 | 编辑 10MB 文件不卡顿 |
| M2 | 行缓存渲染、Monarch 高亮、主题 | 1600x900 单帧 < 33ms（TCG） |
| M3 | 查找替换、命令面板、快捷键、标签/文件树 | 日常编辑闭环 |
| M4 | Oniguruma 子集、TextMate 语法 | 第三方语法可用 |
| M5 | diff、minimap、split、大文件模式 | 对标 VSCode 基础体验 |

当前从 M0 开始。
