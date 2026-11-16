# SLYZ v2 语言参考

SLYZ 是 SaylterOS 的应用语言：源码写成 `.slyzcode`，编译成
`.slyz`（SLYZBIN v2 字节码），由内核虚拟机加载执行。

一个应用一个文件、一个独立虚拟机实例。应用崩溃只终止自己的
虚拟机，桌面、其他应用与内核继续运行。

## 工具链

```text
tools/slyzc/build.ps1                编译 slyzc.exe（编译器 + 反汇编器 + 宿主运行时）
tools/slyzc/build/slyzc.exe compile app.slyzcode -o app.slyz
tools/slyzc/build/slyzc.exe run app.slyz
tools/slyzc/build/slyzc.exe dump app.slyz
```

根目录 `build.ps1` 会自动编译 `samples/*.slyzcode`，并用
`tools/mkfs` 把它们写进系统盘的 `system\apps\`。

## 语法

```slyz
// 变量
let count = 0
let title = "Settings"
let items = [1, 2, 3]
let empty = nil

// 函数
fn add(a, b) {
    return a + b
}

// 控制流
if count > 0 {
    count += 1
} else {
    count = 1
}

while count < 10 {
    count += 1
}

for i in 0..items.length {
    println(items.at(i))
}

// 字符串与方法
let text = "Hello"
println(text.upper() + " " + str(len(text)))
```

运算符优先级从低到高：

| 级别 | 运算符 |
|---|---|
| 赋值 | `=` `+=` `-=` `*=` `/=` `%=` |
| 逻辑或 | `\|\|` `or` |
| 逻辑与 | `&&` `and` |
| 比较 | `==` `!=` `<` `<=` `>` `>=` |
| 加减 | `+` `-` |
| 乘除 | `*` `/` `%` |
| 一元 | `-` `!` `not` |

字符串支持 `\n` `\t` `\r` `\\` `\"` `\0` `\xNN` `\u{1F600}`，
源码本身是 UTF-8，可以直接写中文。

## 类型

- 整数：有符号 64 位
- 字符串：UTF-8 字节序列，索引按字节
- 数组：元素可以是任意类型，支持嵌套
- nil：没有值

真值规则：`0`、空字符串、空数组、`nil` 为假。

## 内置函数

```text
print println eprint
len str int substr index_of upper lower trim replace split join
abs min max clamp sign random array push pop range typeof
str_at ord chr hex bin
```

数组方法：`at(i)` `set(i, v)` `push(v)` `pop()` `length`。

## 文件与配置

```text
fs.read(path)          fs.write(path, text)     fs.exists(path)
fs.list(path)          fs.entries(path)          fs.remove(path)
fs.mkdir(path)
cfg.get(key)           cfg.set(key, value)      cfg.keys()
```

`fs.entries` 返回 `[[name, type, size], ...]`，`type` 为 1（文件）
或 2（目录）。

`cfg` 支持 `boot.mode`、`lang`、`timezone`、`wallpaper`、`avatar`
五个键。`wallpaper` / `avatar` 的值是 SlyzFS 绝对路径（例如
`system\wallpapers\aurora.bmp`），空字符串表示使用内置渐变背景
与首字母头像。修改这两个键会立即触发桌面整屏重绘。

## 时间与进程

```text
time.now() time.ms() time.text() time.date() time.sleep(ms)
os.exec(cmdline) os.exit(code) os.beep(freq, ms)
os.info(key) os.proc(index, field) os.proc_name(index)
```

`os.info` 的 key 与 `os.proc` 的 field 定义在
`src/include/slyz.h` 的 `SLE_INFO_*` / `SLE_PROC_*`。

## 窗口与绘图

```slyz
let handle = win.open("Title", 640, 480)
gfx.clear(color)
gfx.rect(x, y, w, h, color)
gfx.frame(x, y, w, h, color, thickness)
gfx.text(x, y, text, color)
gfx.text_width(text)
gfx.line(x1, y1, x2, y2, color)
gfx.circle(cx, cy, r, color)
gfx.flush()
```

坐标原点在窗口内容区左上角。应用绘制期间内核会把裁剪矩形
钉在内容区，越界绘制不会污染标题栏和其他窗口。

`gfx.flush()` 表示"这一帧画完了"：虚拟机会在这里让出 CPU，
下一帧继续处理事件并绘制下一帧。应用必须每帧调用一次，
否则界面不会刷新。

## 图片

```text
img.load(path)              -> handle，失败返回 -1
img.width(handle)           img.height(handle)
img.draw(handle, x, y)
img.draw_scaled(handle, x, y, w, h)
img.draw_circle(handle, cx, cy, r)
img.free(handle)
```

内核解码 BMP（8 位调色板、16 位 5-6-5 / BI_BITFIELDS、24 位
BGR、32 位 BGRA，自底向上与自顶向下都支持）。解码结果按路径
缓存，同一张图重复 `img.load` 不会重复解码。`draw_circle` 先
把图片居中裁成正方形再映射到圆内，适合做用户头像。

坐标同样是内容区局部坐标；绘制遵守当前窗口裁剪。单张图片
解码后不能超过 3.5MB（约 90 万像素），超出会返回 -1。

设置应用会自动列出 `system\wallpapers` 与 `system\avatars` 下的
BMP，用户把图片放进这两个目录即可出现在列表里。

## 音频与媒体

```text
audio.play(path)            -> 0 成功
audio.stop()
audio.pause(paused)
audio.state()               -> 0 停止 / 1 播放 / 2 暂停
audio.position()            -> 当前毫秒
audio.duration()            -> 总毫秒
audio.set_volume(0..100)    audio.volume()
media.request()             -> 双击 MP3 时桌面写入的待播放路径
os.open(path)               -> 按扩展名打开：.mp3 媒体、.slyz 应用、其他文本
```

内核内置 AC'97 PCM 输出（48 kHz / 16 位 / 立体声）与自研
MPEG-1 Layer III 解码器，MP3 的采样率会线性重采样到 48 kHz。
`audio.play` 只接受 MPEG-1 Layer III（32/44.1/48 kHz），损坏帧
会被跳过；没有声卡时接口仍然可用，只是没有声音。

媒体播放器 `samples/media.slyzcode` 不出现在应用启动器里，只有
双击 `.mp3` 时才会被 `media.request()` 的待播放路径拉起。关闭
播放器窗口会同时停止音频。

## 事件

```slyz
let ev = event.poll()
if ev != nil {
    let kind = ev.at(0)
}
```

| kind | 含义 | 数组内容 |
|---|---|---|
| 1 | 键盘 | `[1, 字符码, 0, 0]` |
| 2 | 鼠标移动 | `[2, x, y, buttons]` |
| 3 | 点击 | `[3, x, y, button]` |
| 4 | 滚轮 | `[4, delta, 0, 0]` |
| 5 | 关闭 | `[5, 0, 0, 0]` |
| 7 | 尺寸变化 | `[7, width, height, 0]` |

`event.wait(ms)` 与 `poll` 一样是非阻塞的：没有事件时返回 `nil`。
需要节流的应用用 `time.ms()` 自己控制节奏，或依赖每帧一次的
`gfx.flush()` 节奏。

## 镜像格式

```text
offset 0   magic[8]       "SLYZBIN\0"
offset 8   u16 version    2
offset 10  u16 flags      bit0 = 使用窗口/绘图
offset 12  u32 const_size
offset 16  u32 code_size
offset 20  u32 entry
offset 24  u32 checksum   FNV-1a(const_pool || code)
offset 28  u32 globals
offset 32  u32 stack_hint
offset 36  u32 source_size
offset 40  u32 reserved0
offset 44  u32 reserved1
offset 48  const_pool
之后       code
```

指令集与操作码见 `src/include/slyz.h`。编译器
`tools/slyzc/slyzc_gen.c` 负责生成，内核与宿主运行时共用
`src/kernel/slyzvm.c` 的解释器。

## 示例

- `samples/hello.slyzcode`：语言特性自检
- `samples/textstats.slyzcode`：字符串与数组
- `samples/calc.slyzcode`：桌面计算器（窗口 + 鼠标 + 键盘）
- `samples/settings.slyzcode`：设置页（cfg 读写、壁纸与头像选择）
- `samples/media.slyzcode`：媒体播放器（audio/media 接口）
- `samples/taskmgr.slyzcode`：任务管理器（os.info/os.proc）
- `samples/files.slyzcode`：文件管理器（fs.entries/fs.remove）
- `samples/gui-window.slyzcode`：最小窗口骨架

## 旧格式说明

SLYZBIN v1（`tools/windows-slyz-tools/`）与文本 `.slyz` 脚本已
废弃，v2 虚拟机不再接受它们的指令集。旧的 `#gui` 声明式界面
（`tools/slyzc.ps1`）也不是 SLYZ v2 程序，仅作为历史工具保留。
