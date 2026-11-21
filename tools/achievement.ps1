# ============================================================
# achievement.ps1 - SaylterOS 成就报告
# ------------------------------------------------------------
# 统计项目年龄、满月状态、恐龙击退次数与已完成里程碑。
#
# 用法：
#   .\achievement.ps1                 查看成就报告
#   .\achievement.ps1 -Repel "原因"   记录一次击退恐龙
#   .\achievement.ps1 -Plain          纯文本（无颜色）
#   .\achievement.ps1 -Now "2026-11-20 20:00"   指定时间（测试用）
#
# 恐龙日志：build\achievement.log（每行 时间|战绩）。
# 首次运行时用下面内置的真实修复记录初始化。
# ============================================================

param(
    [string]$Repel = '',
    [datetime]$Now = (Get-Date),
    [switch]$Plain
)

$ErrorActionPreference = 'Stop'

# 项目起点：2026-10-17 00:00（本地时间）
$Start = [datetime]::new(2026, 10, 17, 0, 0, 0)

$root = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $root 'build'
$logPath = Join-Path $logDir 'achievement.log'

if (!(Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

# ------------------------------------------------------------
# 内置的初始战绩：本次会话真实修复过的问题
# ------------------------------------------------------------
$seed = @(
    '2026-11-20|Slyz v3 编译器补齐位运算（& | ^ ~ << >>）',
    '2026-11-20|slyz3c --baremetal：MS x64 ABI 与 16 字节栈对齐',
    '2026-11-20|裸 LBA 加载 .slys：修正 64 字节头偏移丢前 448 字节',
    '2026-11-20|目录记录 in-use 位写错（1+is_dir）',
    '2026-11-20|OPEN(w) 新建记录后未提交就回读',
    '2026-11-20|切换卷未重读卷头，超级块被串写',
    '2026-11-20|卷头 mft_next 更新后未重算 CRC',
    '2026-11-20|位图逐位写日志撑爆 32 页事务缓冲',
    '2026-11-20|QEMU 64MB 内存装不下 48MB 模块堆',
    '2026-11-20|ATA 逐扇区 flush 与 LBA48 / IDENTIFY 容量探测',
    '2026-11-20|脱离 FAT：MFT + 位图 + 索引块 + 环形日志'
)

if (!(Test-Path $logPath)) {
    Set-Content -Path $logPath -Value $seed -Encoding UTF8
}

if ($Repel -ne '') {
    $stamp = $Now.ToString('yyyy-MM-dd HH:mm')
    $line = '{0}|{1}' -f $stamp, $Repel
    Add-Content -Path $logPath -Value $line -Encoding UTF8
}

$logLines = @(Get-Content -Path $logPath -Encoding UTF8 |
              Where-Object { $_.Trim() -ne '' })

# ------------------------------------------------------------
# 已完成的里程碑
# ------------------------------------------------------------
$milestones = @(
    'SlyerType 平滑字体（可变字重 + CJK 基线）',
    'AHCI / SATA / USB MSC 多后端块设备',
    '早期调试控制台（HUD / 串口落盘 / 安全格式化）',
    'ls / dir 隐藏细节与 SlyzOS 私有字形',
    '中文文件名全链路 UTF-8',
    'Slyz v3 编译到 x86-64 裸机（不依赖运行时）',
    'SlyzFS v2：MFT + 位图 + 索引块 + 环形日志',
    'Windows 风格分区 C: / R: / D:，R: 平时只读',
    '机械盘健壮性：LBA48 + 写缓存 + 提交点 flush',
    '255 字节长文件名与 280 字节目录项',
    '内核桥接：文件系统以 .slys 模块运行',
    '128MB 系统镜像与构建期自动预置'
)

# ------------------------------------------------------------
# 时间计算
# ------------------------------------------------------------
$span = $Now - $Start
$totalDays = $span.TotalDays
$totalHours = $span.TotalHours
$ageYears = $totalDays / 365.2425
$ageMonths = $totalDays / 30.436875

$fullMoonAt = $Start.AddMonths(1)
$isFullMoon = $Now -ge $fullMoonAt
if ($isFullMoon) {
    $fullMoonOver = [math]::Floor(($Now - $fullMoonAt).TotalDays)
    $fullMoonText = '已满月（超出 {0} 天）' -f $fullMoonOver
} else {
    $fullMoonLeft = [math]::Ceiling(($fullMoonAt - $Now).TotalDays)
    $fullMoonText = '未满月（还差 {0} 天）' -f $fullMoonLeft
}

$nextHundred = $Start.AddDays(100)
$hundredText = if ($Now -ge $nextHundred) {
    '已过百天'
} else {
    '距百天还有 {0} 天' -f [math]::Ceiling(($nextHundred - $Now).TotalDays)
}

$dinoCount = $logLines.Count
$dinoRate = if ($dinoCount -gt 0) { $totalDays / $dinoCount } else { 0 }
$lastDino = if ($dinoCount -gt 0) { $logLines[$logLines.Count - 1] } else { '暂无记录' }
$lastDinoText = $lastDino -replace '\|', ' · '

$done = $milestones.Count
$total = $milestones.Count
$barWidth = 24
$filled = [math]::Round($barWidth * $done / $total)
$bar = ('#' * $filled) + ('-' * ($barWidth - $filled))

# ------------------------------------------------------------
# 输出
# ------------------------------------------------------------
function Write-Line {
    param([string]$Text, [ConsoleColor]$Color = [ConsoleColor]::Gray)
    if ($Plain) {
        Write-Output $Text
    } else {
        Write-Host $Text -ForegroundColor $Color
    }
}

$rule = '=' * 64
$thin = '-' * 64

Write-Line $rule Cyan
Write-Line '  SAYLTER OS  ·  成就报告' Cyan
Write-Line $rule Cyan
Write-Line ('  项目起点    {0:yyyy-MM-dd}' -f $Start) Gray
Write-Line ('  当前时间    {0:yyyy-MM-dd HH:mm}' -f $Now) Gray
Write-Line ('  已运行      {0} 天 {1} 小时（{2:N1} 天）' -f
            [math]::Floor($totalDays), [math]::Floor($totalHours % 24), $totalDays) White
Write-Line ('  项目年龄    {0:N3} 岁 / {1:N2} 个月' -f $ageYears, $ageMonths) Yellow
Write-Line ('  满月状态    {0}' -f $fullMoonText) Green
Write-Line ('  百天计划    {0}' -f $hundredText) Green
Write-Line $thin DarkGray
Write-Line ('  恐龙击退    {0} 次' -f $dinoCount) Magenta
if ($dinoRate -gt 0) {
    Write-Line ('  平均频率    每 {0:N1} 天一次' -f $dinoRate) DarkMagenta
}
Write-Line ('  最近战绩    {0}' -f $lastDinoText) DarkMagenta
Write-Line $thin DarkGray
Write-Line '  已完成里程碑' Cyan
foreach ($m in $milestones) {
    Write-Line ('   [x] {0}' -f $m) Green
}
Write-Line $thin DarkGray
Write-Line ('  进度        {0}/{1}    [{2}]' -f $done, $total, $bar) Yellow
Write-Line $rule Cyan
if ($Repel -ne '') {
    Write-Line ('  已记录新战绩：{0}' -f $Repel) Magenta
}
