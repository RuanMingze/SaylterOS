# ============================================================
# SlyzFs build.ps1 - 编译 Windows IFS 驱动
# ------------------------------------------------------------
# 需要 MSVC（VS2022/2026）与 WDK 头/库。WDK 未安装时脚本会
# 打印 NuGet 补齐方法后退出，不会破坏现有构建。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File drivers\slyzfs\build.ps1
#   ... -Sign        编译后自动调用 tools\driversign\sign-driver.ps1
#   ... -WdkRoot D:\wdk    手动指定 WDK 根（含 Include\<ver>\km）
# ============================================================

param(
    [string]$VsRoot = 'D:\Program Files\Microsoft Visual Studio\18\Community',
    [string]$WdkRoot = '',
    [switch]$Sign
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$OutDir = Join-Path $Repo 'build\drivers'
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Find-VsRoot {
    param([string]$Preferred)
    if ($Preferred -and (Test-Path (Join-Path $Preferred 'VC\Tools\MSVC'))) {
        return $Preferred
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $p -and (Test-Path (Join-Path $p 'VC\Tools\MSVC'))) {
            return $p.Trim()
        }
    }
    $roots = @('D:\Program Files\Microsoft Visual Studio', 'C:\Program Files\Microsoft Visual Studio',
               'D:\Program Files (x86)\Microsoft Visual Studio', 'C:\Program Files (x86)\Microsoft Visual Studio')
    foreach ($r in $roots) {
        if (!(Test-Path $r)) { continue }
        $hit = Get-ChildItem -Path $r -Directory -ErrorAction SilentlyContinue |
               Where-Object { Test-Path (Join-Path $_.FullName 'VC\Tools\MSVC') } |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return ''
}

function Find-KmRoot {
    param([string]$Preferred, [string[]]$Roots)
    $candidates = @()
    if ($Preferred) { $candidates += $Preferred }
    if ($env:WDKContentRoot) { $candidates += $env:WDKContentRoot }
    $candidates += $Roots
    foreach ($root in $candidates) {
        if (!$root -or !(Test-Path $root)) { continue }
        $incRoot = Join-Path $root 'Include'
        if (Test-Path $incRoot) {
            $vers = Get-ChildItem -Path $incRoot -Directory -ErrorAction SilentlyContinue |
                    Sort-Object Name -Descending
            foreach ($v in $vers) {
                $km = Join-Path $v.FullName 'km'
                if (Test-Path (Join-Path $km 'ntifs.h')) {
                    return @{ Root = $root; Version = $v.Name; Km = $km }
                }
            }
        }
        $found = Get-ChildItem -Path $root -Filter ntifs.h -Recurse -ErrorAction SilentlyContinue |
                 Select-Object -First 1
        if ($found) {
            $km = Split-Path -Parent $found.FullName
            $verDir = Split-Path -Parent $km
            $incDir = Split-Path -Parent $verDir
            $rootReal = Split-Path -Parent $incDir
            return @{ Root = $rootReal; Version = (Split-Path -Leaf $verDir); Km = $km }
        }
    }
    return $null
}

$vs = Find-VsRoot $VsRoot
if (!$vs) {
    Write-Host 'MSVC not found. Pass -VsRoot <VS install path>.' -ForegroundColor Red
    exit 1
}
$cl = Get-ChildItem -Path (Join-Path $vs 'VC\Tools\MSVC') -Directory |
       Sort-Object Name -Descending |
       ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe' } |
       Where-Object { Test-Path $_ } | Select-Object -First 1
$link = $cl -replace 'cl\.exe$', 'link.exe'
if (!$cl -or !(Test-Path $link)) {
    Write-Host "cl.exe/link.exe not found under $vs" -ForegroundColor Red
    exit 1
}

$rootList = @('D:\Windows Kits\10', 'C:\Program Files (x86)\Windows Kits\10',
              (Join-Path $Repo 'build\wdk'))
$km = Find-KmRoot $WdkRoot $rootList
if (!$km) {
    Write-Host 'WDK kernel headers (ntifs.h) not found.' -ForegroundColor Red
    Write-Host '安装方式（免管理员）：'
    Write-Host '  1) 下载 https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/10.0.26100.6584/microsoft.windows.wdk.x64.10.0.26100.6584.nupkg'
    Write-Host '  2) 解压到 build\wdk\ 后重跑本脚本'
    exit 0
}

$shared = Join-Path (Split-Path -Parent $km.Km) 'shared'
$ucrt   = Join-Path (Split-Path -Parent $km.Km) 'ucrt'
$vcInc  = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $cl)))) 'include'
if (!(Test-Path (Join-Path $vcInc 'excpt.h'))) {
    Write-Host "MSVC include (excpt.h) not found: $vcInc" -ForegroundColor Red
    exit 1
}
$kmLib  = Join-Path $km.Root ("Lib\$($km.Version)\km\x64")
if (!(Test-Path $kmLib)) {
    $hit = Get-ChildItem -Path $km.Root -Recurse -Filter ntoskrnl.lib -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($hit) { $kmLib = Split-Path -Parent $hit.FullName }
}

Write-Host "MSVC : $cl"
Write-Host "WDK  : $($km.Root) ($($km.Version))"
Write-Host "Lib  : $kmLib"

$inc = @("/I$($km.Km)", "/I$shared", "/I$ucrt", "/I$vcInc")
$sources = @((Join-Path $Repo 'drivers\slyzfs\SlyzFs.c'),
             (Join-Path $Repo 'drivers\slyzfs\SlyzFsWrite.c'))
$objs = @()

foreach ($src in $sources) {
    $obj = Join-Path $OutDir ((Split-Path -Leaf $src) -replace '\.c$', '.obj')
    $clArgs = @('/nologo', '/c', '/kernel', '/O2', '/W3', '/GS-', '/Gs999999', '/utf-8',
              '/std:c11', '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DNT=1', '/D_KERNEL_',
              "/Fo$obj") + $inc + @($src)
    & $cl @clArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "compile failed: $src" -ForegroundColor Red
        exit 1
    }
    $objs += $obj
}

$sys = Join-Path $OutDir 'SlyzFs.sys'
$lnk = @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE', '/ENTRY:DriverEntry',
         '/MACHINE:X64', '/NODEFAULTLIB', '/INTEGRITYCHECK',
         "/OUT:$sys", "/LIBPATH:$kmLib") + $objs + @('ntoskrnl.lib', 'hal.lib')
& $link @lnk
if ($LASTEXITCODE -ne 0) {
    Write-Host 'link failed.' -ForegroundColor Red
    exit 1
}

Write-Host "built: $sys" -ForegroundColor Green

if ($Sign) {
    $signScript = Join-Path $Repo 'tools\driversign\sign-driver.ps1'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $signScript -In $sys
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'sign failed.' -ForegroundColor Red
        exit 1
    }
}
