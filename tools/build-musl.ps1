# ============================================================
# build-musl.ps1 - 构建 musl 静态应用（.slys）
# ------------------------------------------------------------
# musl 移植属于 SaylterOS 的源码，不是 Slyztrix 的：
#   third_party\musl   上游 1.2.5 源码（不改）
#   src\slyzlibc       SaylterOS 适配层（syscall / crt / ABI 桥 / 链接脚本）
#   examples\<App>.c   应用入口
# Slyztrix 只保留进度报告（README / pledge.html）。
#
# 这个脚本固化整条链路，避免每次手工敲一长串命令：
#   1. 可选 -Bootstrap：configure + make 编译 musl 源码，分批归档 libc.a
#      （Windows 的单条命令行长度上限约 32K，1345 个对象的 ar 必须分批）
#   2. 编译适配层（crt / syscall / exit / msabi）与 examples\<App>.c
#   3. 按 src\slyzlibc\linker.ld 链接到固定基址 0x09000000
#   4. objcopy 出平坦二进制，并用 nm 读出入口偏移与 BSS 大小
#   5. 调用 tools\mkslys 打上 64 字节头，产出 <App>.slys
#
# 用法：
#   .\tools\build-musl.ps1 -App hello
#   .\tools\build-musl.ps1 -App hello -Bootstrap
#   .\tools\build-musl.ps1 -App hello -OutDir build
# ============================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$App,

    # .slys 输出目录；默认输出到仓库根的 build 目录
    [string]$OutDir = '',

    # x86_64-elf 交叉工具链根目录（其下有 bin\x86_64-elf-gcc.exe）
    [string]$ToolchainRoot = 'E:\dev\x86_64-elf-tools-windows',

    # mkslys.exe 路径；为空时依次尝试 build\mkslys.exe 与本地编译
    [string]$Mkslys = '',

    # SaylterOS 仓库根目录；默认由脚本自身位置推导，一般不需要传
    [string]$SaylterRoot = '',

    # 重新 configure + 重新编译 musl 源码
    [switch]$Bootstrap,

    # 无条件重新编译 port 层与应用程序
    [switch]$Force,

    # 打开 port 层的串口追踪（-DSLYZ_TRACE=1）
    [switch]$Trace
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
if (-not $SaylterRoot) {
    $SaylterRoot = $root
}
if (-not $OutDir) {
    $OutDir = Join-Path $root 'build'
}

$muslDir = Join-Path $root 'third_party\musl'
$portDir = Join-Path $root 'src\slyzlibc'
$buildDir = Join-Path $root 'build\musl'
$objDir = Join-Path $buildDir 'obj'
$portObjDir = Join-Path $buildDir 'port'

foreach ($d in @($buildDir, $objDir, $portObjDir, $OutDir)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}

# ---------- 工具链 ----------
$tcBin = Join-Path $ToolchainRoot 'bin'
$cc = Join-Path $tcBin 'x86_64-elf-gcc.exe'
$ar = Join-Path $tcBin 'x86_64-elf-ar.exe'
$ranlib = Join-Path $tcBin 'x86_64-elf-ranlib.exe'
$objcopy = Join-Path $tcBin 'x86_64-elf-objcopy.exe'
$nm = Join-Path $tcBin 'x86_64-elf-nm.exe'
$size = Join-Path $tcBin 'x86_64-elf-size.exe'

foreach ($t in @($cc, $ar, $ranlib, $objcopy, $nm, $size)) {
    if (-not (Test-Path $t)) {
        throw "找不到交叉工具链组件：$t（用 -ToolchainRoot 指定根目录）"
    }
}

$msysBin = 'D:\msys2-64\usr\bin'
$make = Join-Path $msysBin 'make.exe'
$sh = Join-Path $msysBin 'sh.exe'

# mkslys.exe 由 ucrt64 g++ 构建，动态链接 ucrt64 运行库。PATH 里若先
# 命中 mingw64/mingw32 的同名 DLL，进程会以 0xC0000139 退出。
$ucrtBin = 'D:\msys2-64\ucrt64\bin'
if ((Test-Path $ucrtBin) -and (($env:PATH -split ';') -notcontains $ucrtBin)) {
    $env:PATH = "$ucrtBin;$env:PATH"
}

$env:PATH = "$tcBin;$msysBin;$env:PATH"

function Write-Step([string]$Text) {
    Write-Host "===== $Text =====" -ForegroundColor Cyan
}

function Write-Ok([string]$Text) {
    Write-Host "  OK $Text" -ForegroundColor Green
}

function Write-Fail([string]$Text) {
    Write-Host "  FAILED $Text" -ForegroundColor Red
}

# ============================================================
# 1. musl 源码构建
# ============================================================

# musl 的 syscall 覆盖头必须排在所有系统头之前；configure 只负责生成
# config.mak 的其余字段，CFLAGS_C99FSE 由这里改写为确定内容，保证每次
# 从任意路径克隆仓库都能得到同样的编译参数。
$cfgPortInclude = ($portDir -replace '\\', '/')
$cfgCFlags = "-I$cfgPortInclude -mno-red-zone -mno-stack-arg-probe " +
             "-mcmodel=small -fno-stack-protector -std=c99 -nostdinc " +
             "-ffreestanding -fexcess-precision=standard -frounding-math " +
             "-fno-strict-aliasing -Wa,--noexecstack"

function Invoke-MuslConfigure {
    if (-not (Test-Path $sh)) { throw "找不到 MSYS2 sh：$sh" }
    $log = Join-Path $buildDir 'musl-configure.log'
    $cmd = "CC=x86_64-elf-gcc AR=x86_64-elf-ar RANLIB=x86_64-elf-ranlib " +
           "sh ./configure --target=x86_64-slyz --disable-shared --disable-debug"
    Push-Location $muslDir
    try {
        & $sh -c $cmd *> $log
        if ($LASTEXITCODE -ne 0) {
            Get-Content $log -Tail 40 | ForEach-Object { Write-Host "    $_" }
            throw "musl configure 失败（日志：$log）"
        }
    } finally {
        Pop-Location
    }
    Write-Ok 'musl configure'
}

function Set-MuslCFlags {
    $cfgPath = Join-Path $muslDir 'config.mak'
    if (-not (Test-Path $cfgPath)) { throw "缺少 $cfgPath" }
    $lines = Get-Content $cfgPath
    $out = New-Object System.Collections.Generic.List[string]
    $replaced = $false
    foreach ($line in $lines) {
        if ($line -match '^CFLAGS_C99FSE\s*=') {
            $out.Add("CFLAGS_C99FSE = $cfgCFlags")
            $replaced = $true
        } else {
            $out.Add($line)
        }
    }
    if (-not $replaced) {
        throw 'config.mak 中没有 CFLAGS_C99FSE，configure 版本不匹配'
    }
    $text = ($out -join "`n") + "`n"
    Set-Content -Path $cfgPath -Value $text -NoNewline -Encoding ascii
    Write-Ok 'config.mak 的 CFLAGS_C99FSE 已按 port 覆盖头重写'
}

function Invoke-MuslMake {
    if (-not (Test-Path $make)) { throw "找不到 MSYS2 make：$make" }
    $log = Join-Path $buildDir 'musl-make.log'
    Push-Location $muslDir
    try {
        # -k：ar 因命令行过长失败后继续完成其余目标，对象文件才是这里要的
        & $make -k -j8 *> $log
    } finally {
        Pop-Location
    }

    $objects = @(Get-ChildItem -Path $muslDir -Recurse -Filter '*.o' -File |
                 Where-Object { $_.FullName -like "*\obj\*" })
    if ($objects.Count -lt 1000) {
        Get-Content $log -Tail 60 | ForEach-Object { Write-Host "    $_" }
        throw "musl 目标文件数量异常：$($objects.Count)（日志：$log）"
    }
    Write-Ok "musl 对象文件 $($objects.Count) 个"

    $libDir = Join-Path $muslDir 'lib'
    if (-not (Test-Path $libDir)) { New-Item -ItemType Directory -Path $libDir | Out-Null }
    $libc = Join-Path $libDir 'libc.a'
    if (Test-Path $libc) { Remove-Item $libc -Force }

    # 分批归档：Windows 单条命令行约 32K 字符，一次传 1345 个路径必然失败。
    # 每批 100 个，先 rcs 建立索引，后续批次用 r 覆盖同名成员。
    $batch = 100
    $files = @($objects | ForEach-Object { $_.FullName })
    $batches = [int][Math]::Ceiling($files.Count / $batch)
    for ($i = 0; $i -lt $batches; $i++) {
        $from = $i * $batch
        $take = [Math]::Min($batch, $files.Count - $from)
        $slice = $files[$from..($from + $take - 1)]
        if ($i -eq 0) {
            $r = Invoke-Native -Exe $ar -Arguments (@('rcs', $libc) + $slice)
        } else {
            $r = Invoke-Native -Exe $ar -Arguments (@('r', $libc) + $slice)
        }
        if ($r.Code -ne 0) {
            $r.Output | Select-Object -First 20 | ForEach-Object { Write-Host "    $_" }
            throw "ar 第 $($i + 1) 批失败"
        }
    }
    $r = Invoke-Native -Exe $ranlib -Arguments @($libc)
    if ($r.Code -ne 0) { throw 'ranlib 失败' }

    # Windows 文件系统大小写不敏感，src\exit\_Exit.c 与 src\unistd\_exit.c
    # 都会生成 _Exit.o/_exit.o，归档时后写入的会覆盖前者。链接期由
    # port\exit.c 强定义 _Exit 补齐，这里显式确认冲突确实存在。
    $exitMembers = @((Invoke-Native -Exe $ar -Arguments @('t', $libc)).Output |
                     Where-Object { $_ -match '^_exit\.o$' })
    Write-Ok ("libc.a {0:N0} 字节（_exit.o 成员 {1} 个）" -f (Get-Item $libc).Length, $exitMembers.Count)
}

function Ensure-MuslLib {
    $libc = Join-Path $muslDir 'lib\libc.a'
    $cfgPath = Join-Path $muslDir 'config.mak'
    $portCfg = ($portDir -replace '\\', '/') + '/include'
    $needBootstrap = $Bootstrap.IsPresent

    if (-not (Test-Path (Join-Path $muslDir 'configure'))) {
        throw ("缺少 musl 源码：{0}。它随 SaylterOS 仓库一起提供，" -f $muslDir) +
              '只需 clone 本仓库，不需要额外下载或 clone。'
    }

    # config.mak 记录的是绝对路径。仓库搬家或换机器后会失效：
    # 已有 libc.a 时只重写这一行，没有 libc.a 才完整重建。
    if ((Test-Path $cfgPath) -and
        ((Get-Content -Raw $cfgPath) -notmatch [regex]::Escape("-I$portCfg"))) {
        if (Test-Path $libc) {
            Set-MuslCFlags
        } else {
            $needBootstrap = $true
        }
    }

    if ($needBootstrap -or -not (Test-Path $libc)) {
        Write-Step 'musl 源码构建'
        Invoke-MuslConfigure
        Set-MuslCFlags
        Invoke-MuslMake
    }
    if (-not (Test-Path $libc)) { throw "缺少 $libc" }
    return $libc
}

# ============================================================
# 2. 编译 port 层与应用程序
# ============================================================

# include 顺序与 musl 构建时一致：src/include 必须在 include 之前，
# 否则 features.h 会取错（hidden 等内部宏定义在 src/include 里）。
$includes = @(
    "-I$(Join-Path $muslDir 'src\include')",
    "-I$(Join-Path $muslDir 'src\internal')",
    "-I$(Join-Path $muslDir 'obj\include')",
    "-I$(Join-Path $muslDir 'include')",
    "-I$(Join-Path $portDir 'include')",
    "-I$(Join-Path $muslDir 'arch\x86_64')",
    "-I$(Join-Path $muslDir 'arch\generic')"
)

$cflags = @(
    '-ffreestanding', '-nostdinc', '-m64', '-mno-red-zone',
    '-mno-stack-arg-probe', '-fno-stack-protector', '-mcmodel=small',
    '-O2', '-fno-pic', '-fno-asynchronous-unwind-tables',
    '-fno-unwind-tables', '-Wall', '-Wextra'
)
if ($Trace) { $cflags += '-DSLYZ_TRACE=1' }

# 编译参数指纹：参数变化（例如开关 -Trace）时必须强制重编，
# 否则会继续沿用上一次编译出来的对象文件，得到的二进制与当前
# 参数不一致。
$stampPath = Join-Path $portObjDir 'cflags.stamp'
$stampNow = ($cflags -join ' ') + '|' + ($includes -join ' ')
$stampOld = if (Test-Path $stampPath) { Get-Content -Raw $stampPath } else { '' }
$needForce = $Force.IsPresent
if ($stampOld -ne $stampNow) {
    $needForce = $true
    Set-Content -Path $stampPath -Value $stampNow -NoNewline -Encoding ascii
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    # PowerShell 5.1 会把原生命令的 stderr 当成 ErrorRecord；在
    # $ErrorActionPreference='Stop' 下会直接抛异常并吞掉真正的编译错误。
    # 这里统一收集输出与退出码，由调用方决定如何呈现。
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $Exe @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    return [pscustomobject]@{ Code = $code; Output = @($output) }
}

function Invoke-Compile([string]$Source, [string]$Object) {
    $need = $needForce
    if (-not $need) {
        if (-not (Test-Path $Object)) {
            $need = $true
        } else {
            $need = (Get-Item $Source).LastWriteTimeUtc -gt (Get-Item $Object).LastWriteTimeUtc
            if (-not $need) {
                foreach ($h in @('slyzposix.h', 'syscall_arch.h')) {
                    $hp = Join-Path $portDir "include\$h"
                    if ((Test-Path $hp) -and
                        (Get-Item $hp).LastWriteTimeUtc -gt (Get-Item $Object).LastWriteTimeUtc) {
                        $need = $true
                    }
                }
            }
        }
    }
    if (-not $need) { return }
    $r = Invoke-Native -Exe $cc -Arguments ($cflags + $includes + @('-c', $Source, '-o', $Object))
    if ($r.Code -ne 0) {
        $r.Output | Select-Object -First 40 | ForEach-Object { Write-Host "    $_" }
        throw "编译失败：$Source"
    }
    $r.Output | Where-Object { $_ -match 'warning' } |
        ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
}

function Build-App([string]$AppName) {
    Write-Step "编译 musl 应用：$AppName"

    $srcApp = Join-Path $root "examples\$AppName.c"
    if (-not (Test-Path $srcApp)) { throw "找不到应用源码：$srcApp" }

    $crtObj = Join-Path $portObjDir 'crt.o'
    $sysObj = Join-Path $portObjDir 'syscall.o'
    $exitObj = Join-Path $portObjDir 'exit.o'
    $abiObj = Join-Path $portObjDir 'msabi.o'
    $appObj = Join-Path $objDir "$AppName.o"

    Invoke-Compile (Join-Path $portDir 'crt.c') $crtObj
    Invoke-Compile (Join-Path $portDir 'syscall.c') $sysObj
    Invoke-Compile (Join-Path $portDir 'exit.c') $exitObj
    Invoke-Compile (Join-Path $portDir 'msabi.s') $abiObj
    Invoke-Compile $srcApp $appObj

    $elf = Join-Path $buildDir "$AppName.elf"
    $bin = Join-Path $buildDir "$AppName.bin"
    $libc = Join-Path $muslDir 'lib\libc.a'
    $ldArgs = @(
        '-nostdlib', '-m64', '-Wl,--no-warn-rwx-segments',
        '-T', (Join-Path $portDir 'linker.ld'),
        $crtObj, $sysObj, $exitObj, $abiObj, $appObj,
        $libc, '-lgcc', '-o', $elf
    )
    $r = Invoke-Native -Exe $cc -Arguments $ldArgs
    if ($r.Code -ne 0) {
        $r.Output | Select-Object -First 40 | ForEach-Object { Write-Host "    $_" }
        throw "链接失败：$elf"
    }
    $r.Output | Where-Object { $_ -match 'warning' } |
        ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }

    $r = Invoke-Native -Exe $objcopy -Arguments @('-O', 'binary', $elf, $bin)
    if ($r.Code -ne 0) { throw "objcopy 失败：$bin" }

    $syms = (Invoke-Native -Exe $nm -Arguments @($elf)).Output
    $addr = @{}
    foreach ($line in $syms) {
        if ($line -match '^([0-9A-Fa-f]{16})\s+\S\s+(\S+)$') {
            $addr[$Matches[2]] = [Convert]::ToUInt64($Matches[1], 16)
        }
    }
    foreach ($s in @('slyzposix_entry', '__bss_start', '__bss_end')) {
        if (-not $addr.ContainsKey($s)) { throw "缺少符号 $s" }
    }

    $loadAddr = 0x09000000
    $entryOff = $addr['slyzposix_entry'] - $loadAddr
    $bssSize = $addr['__bss_end'] - $addr['__bss_start']
    $imgSize = (Get-Item $bin).Length
    $expectImg = $addr['__bss_start'] - $loadAddr
    # objcopy 只写到最后一个"已加载"段，末尾与 __bss_start 之间的对齐空隙
    # 不会出现在 .bin 里。加载器从 image_size 处开始按 bss_size 清零，
    # 正好覆盖这段空隙，所以 image < __bss_start 是正常情况；
    # 反过来 image 越过 __bss_start 才会把初始化数据写进会被清零的区间。
    if ($imgSize -gt $expectImg) {
        throw ("镜像 {0} 字节越过了 __bss_start（{1} 字节处）" -f $imgSize, $expectImg)
    }
    if ($entryOff -ge $imgSize) { throw "入口偏移越界：0x$('{0:X}' -f $entryOff)" }

    (Invoke-Native -Exe $size -Arguments @($elf)).Output |
        ForEach-Object { Write-Host "  $_" }
    Write-Ok ("{0}.elf 入口 +0x{1:X}，文本 {2:N0}B，BSS {3:N0}B" -f $AppName, $entryOff, $imgSize, $bssSize)

    return @{
        Elf = $elf
        Bin = $bin
        EntryOffset = $entryOff
        BssSize = $bssSize
        ImageSize = $imgSize
        LoadAddr = $loadAddr
    }
}

# ============================================================
# 3. mkslys：打 .slys 头
# ============================================================

function Resolve-Mkslys {
    if ($Mkslys) {
        if (-not (Test-Path $Mkslys)) { throw "指定的 mkslys 不存在：$Mkslys" }
        return (Resolve-Path $Mkslys).Path
    }
    $candidates = @(
        (Join-Path $SaylterRoot 'build\mkslys.exe'),
        (Join-Path $buildDir 'mkslys.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }

    $src = Join-Path $SaylterRoot 'tools\mkslys\mkslys.cpp'
    if (-not (Test-Path $src)) {
        throw "找不到 mkslys.exe，也没有源码：$src"
    }
    $gpp = 'D:\msys2-64\ucrt64\bin\g++.exe'
    if (-not (Test-Path $gpp)) { throw "需要 g++ 编译 mkslys：$gpp" }
    # SaylterOS\build 存在时把 mkslys 放在那里，与内核构建产物保持一致。
    $slyBuild = Join-Path $SaylterRoot 'build'
    $outExe = if (Test-Path $slyBuild) {
        Join-Path $slyBuild 'mkslys.exe'
    } else {
        Join-Path $buildDir 'mkslys.exe'
    }
    $r = Invoke-Native -Exe $gpp -Arguments @('-std=c++17', '-O2', '-o', $outExe, $src)
    if ($r.Code -ne 0) {
        $r.Output | Select-Object -First 20 | ForEach-Object { Write-Host "    $_" }
        throw 'mkslys 编译失败'
    }
    return $outExe
}

# ============================================================
# 主流程
# ============================================================

$libcPath = Ensure-MuslLib

$appInfo = Build-App $App

$mkslysExe = Resolve-Mkslys
$slysOut = Join-Path $OutDir "$App.slys"

Write-Step "打包 $App.slys"
$r = Invoke-Native -Exe $mkslysExe -Arguments @(
    $appInfo.Bin, $slysOut, $App,
    ("0x{0:X}" -f $appInfo.LoadAddr),
    ("0x{0:X}" -f $appInfo.EntryOffset),
    ("0x{0:X}" -f $appInfo.BssSize)
)
$r.Output | ForEach-Object { Write-Host "  $_" }
if ($r.Code -ne 0 -or -not (Test-Path $slysOut)) {
    Write-Fail "mkslys 打包 $App"
    exit 1
}

$final = Get-Item $slysOut
Write-Ok ("{0}（{1:N0} 字节 = 64B 头 + {2:N0}B 镜像，BSS {3:N0}B）" -f
          $final.FullName, $final.Length, $appInfo.ImageSize, $appInfo.BssSize)
Write-Host ''
Write-Host "在 SaylterOS 中运行：app $App.slys" -ForegroundColor Yellow
