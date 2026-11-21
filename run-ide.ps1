# ============================================================
# SaylterOS 启动脚本（IDE 镜像）
# ------------------------------------------------------------
# 默认行为：
#   - 从 build\saylter-ide.img 启动（-snapshot，改动不落盘）
#   - 允许 guest 执行 reboot（切换 GUI/TTY 启动模式后必须能重启）
#   - 显式挂 e1000 + user 网络
#   - 串口日志写入 build\serial-run.log
#   - GTK 窗口按屏幕自动缩放
#
# GUI 测试流程：
#   1. .\run-ide.ps1
#   2. 登录 root
#   3. bootmode gui        （或 nano etc\system.cfg 把 boot.mode 改成 gui）
#   4. reboot
#   5. 重启后显示 GUI 启动画面 -> 图形登录 -> 桌面
#   6. 鼠标点击画面可捕获，Ctrl+Alt+G 释放
#
# 持久化测试：
#   .\run-ide.ps1 -Persist
#     使用 build\saylter-test.img，改动保存到该镜像；
#     build.ps1 重新生成 saylter-ide.img 后会自动复制一份新的测试盘。
#
# 其他参数：
#   -MemoryMB 128     调整内存
#   -NoReboot         禁止 guest 重启（QEMU 在 reboot 时退出）
#   -Headless         无窗口 + monitor tcp:4470（自动化用）
#   -UsbDisk          IDE 引导，内核优先从 U 盘读 SlyzFS
#   -UsbOnly          只挂 U 盘：BIOS 与内核都必须走 USB
#   -Usb2             用 EHCI(USB 2.0) 而不是 xHCI(USB 3.0)
#   -Sata             额外挂一块 SATA(AHCI) 磁盘，验证内核 SATA 后端
#   -Hell             地狱模式：q35 + AHCI 512GB 动态稀疏盘 + 4 CPU
#                     + HDA 声卡 + virtio-rng，尽量贴近实体机；
#                     直接写盘（不走 snapshot），用于掉电/日志重放测试
#   -HellDiskGB 512   地狱盘容量（默认 512GB，逻辑大小，实际按写入增长）
#   -Image <path>     直接指定镜像
# ============================================================

param(
    [switch]$Persist,
    [switch]$NoReboot,
    [switch]$Headless,
    [switch]$UsbDisk,
    [switch]$UsbOnly,
    [switch]$Usb2,
    [switch]$Sata,
    [switch]$Hell,
    [switch]$VirtGpu,
    [int]$HellDiskGB = 512,
    [int]$MemoryMB = 256,
    [int]$Smp = 3,
    [string]$Image = ''
)

$ErrorActionPreference = 'Stop'
$bld = 'build'

# 地狱模式：q35 + AHCI 大容量稀疏盘，尽量贴近实体机。
# 盘是动态扩张的：逻辑 512GB，实际只占用已写入的几十 MB。
if ($Hell) {
    if ($Image -eq '') { $Image = "$bld\saylter-hell.img" }
    if ($MemoryMB -lt 2048) { $MemoryMB = 2048 }
    if ($Smp -lt 4) { $Smp = 4 }

    $needBuild = $true
    if (Test-Path $Image) {
        $needBuild = $false
        if (Test-Path "$bld\kernel.bin") {
            $needBuild = (Get-Item $Image).LastWriteTime -lt (Get-Item "$bld\kernel.bin").LastWriteTime
        }
    }
    if ($needBuild) {
        Write-Host ("Building {0}GB hell image: {1}" -f $HellDiskGB, $Image)
        powershell -ExecutionPolicy Bypass -File .\build.ps1 `
            -ImageSizeGB $HellDiskGB -ImagePath $Image
        if ($LASTEXITCODE -ne 0 -or !(Test-Path $Image)) {
            Write-Host 'Hell image build failed.' -ForegroundColor Red
            exit 1
        }
    }
} elseif (-not (Test-Path "$bld\saylter-ide.img")) {
    Write-Host 'IDE image not found, rebuilding...'
    powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
    if (-not (Test-Path "$bld\saylter-ide.img")) {
        Write-Host 'Build failed: build\saylter-ide.img was not produced.' -ForegroundColor Red
        exit 1
    }
}

if ($Image -eq '') {
    if ($Persist) {
        $Image = "$bld\saylter-test.img"
        $src = "$bld\saylter-ide.img"
        $needCopy = $true
        if (Test-Path $Image) {
            $needCopy = (Get-Item $src).LastWriteTime -gt (Get-Item $Image).LastWriteTime
        }
        if ($needCopy) {
            Copy-Item $src $Image -Force
            Write-Host "Created fresh test image: $Image"
        }
    } else {
        $Image = "$bld\saylter-ide.img"
    }
}

if (-not (Test-Path $Image)) {
    Write-Host "Image not found: $Image" -ForegroundColor Red
    exit 1
}

$qemu = $null
$cmd = Get-Command 'qemu-system-x86_64' -ErrorAction SilentlyContinue
if ($cmd) { $qemu = $cmd.Source }
if (-not $qemu) {
    $fallback = 'D:\msys2-64\ucrt64\bin\qemu-system-x86_64.exe'
    if (Test-Path $fallback) { $qemu = $fallback }
}
if (-not $qemu) {
    Write-Host 'qemu-system-x86_64 not found (PATH or D:\msys2-64\ucrt64\bin).' -ForegroundColor Red
    exit 1
}

$serialLog = "$bld\serial-run.log"
Remove-Item $serialLog -ErrorAction SilentlyContinue

if ($Hell) {
    $qargs = @(
        '-machine','q35',
        '-cpu','max',
        '-smp',"$Smp",
        '-m',"$MemoryMB",
        '-boot','order=c',
        '-rtc','base=localtime,driftfix=slew',
        '-global','ICH9-LPC.disable_s3=0'
    )
} else {
    $qargs = @(
        '-machine','pc',
        '-cpu','qemu64',
        '-smp',"$Smp",
        '-m',"$MemoryMB",
        '-boot','c'
    )
}

# 虚拟显卡：virtio-vga 保留 VGA/VBE 兼容层，SeaBIOS 与我们现有的
# VBE 初始化都能工作；后续再为 virtio-gpu 2D 命令加驱动。
if ($VirtGpu) {
    $qargs += @('-vga','none','-device','virtio-vga')
}

# 地狱模式：SATA(AHCI) 512GB 动态盘，写缓存开启；出错要报告而不是吞掉。
if ($Hell) {
    $qargs += @('-device','ich9-ahci,id=ahci')
    $qargs += @('-drive',"if=none,id=hell0,format=raw,file=$Image,cache=writeback,aio=threads,werror=report,rerror=report")
    $qargs += @('-device','ide-hd,drive=hell0,bus=ahci.0')
} elseif (-not $UsbOnly) {
    # IDE 引导盘：-UsbOnly 时不挂载，完全从 USB 启动
    $qargs += @('-drive',"format=raw,file=$Image,if=ide")
}

# USB 存储：qemu-xhci + usb-storage
#   -UsbDisk : IDE 引导，内核优先从 U 盘读 SlyzFS
#   -UsbOnly : 只有 U 盘，BIOS 与内核都必须走 USB
if ($UsbDisk -or $UsbOnly) {
    $usbImg = "$bld\saylter-usb.img"
    $needCopy = $true
    if (Test-Path $usbImg) {
        $needCopy = (Get-Item $Image).LastWriteTime -gt (Get-Item $usbImg).LastWriteTime
    }
    if ($needCopy) {
        Copy-Item $Image $usbImg -Force
        Write-Host "Created fresh USB image: $usbImg"
    }
    if ($Usb2) {
        $qargs += @('-device','usb-ehci,id=ehci')
        $qargs += @('-drive',"if=none,id=usb0,format=raw,file=$usbImg")
        $qargs += @('-device','usb-storage,drive=usb0,bus=ehci.0')
    } else {
        $qargs += @('-device','qemu-xhci,id=xhci')
        $qargs += @('-drive',"if=none,id=usb0,format=raw,file=$usbImg")
        $qargs += @('-device','usb-storage,drive=usb0,bus=xhci.0')
    }
}

# SATA 测试盘：ICH9 AHCI 控制器 + 一块与系统盘内容相同的 SATA 磁盘。
# 内核仍在 IDE 上引导，但 blk.c 会优先选中 AHCI，SlyzFS 实际从
# SATA 盘读写，用来验证 ahci.c 的 DMA 路径。
if ($Sata) {
    $sataImg = "$bld\saylter-sata.img"
    $needCopy = $true
    if (Test-Path $sataImg) {
        $needCopy = (Get-Item $Image).LastWriteTime -gt (Get-Item $sataImg).LastWriteTime
    }
    if ($needCopy) {
        Copy-Item $Image $sataImg -Force
        Write-Host "Created fresh SATA image: $sataImg"
    }
    $qargs += @('-device','ich9-ahci,id=ahci')
    $qargs += @('-drive',"if=none,id=sata0,format=raw,file=$sataImg")
    $qargs += @('-device','ide-hd,drive=sata0,bus=ahci.0')
}

$qargs += @(
    '-device','e1000,netdev=n0',
    '-netdev','user,id=n0',
    '-serial',"file:$($serialLog -replace '\\','/')"
)
# AC'97 声卡：
#   交互模式用 SDL 的 winmm 后端（Windows waveOut，兼容性比
#   DirectSound 更好）；自动化模式用 SDL dummy，保证 DMA 按真实
#   时间推进但不占用宿主音频设备。
if ($Headless) {
    $env:SDL_AUDIODRIVER = 'dummy'
} else {
    $env:SDL_AUDIODRIVER = 'winmm'
}
$qargs += @('-audiodev','sdl,id=snd0')
if ($Hell) {
    # 实体机常见 HDA；同时保留 e1000 网卡与 HPET/ACPI。
    $qargs += @('-device','intel-hda','-device','hda-duplex,audiodev=snd0')
    $qargs += @('-device','virtio-rng-pci')
} else {
    $qargs += @('-device','AC97,audiodev=snd0')
}

if ($Headless) {
    $qargs += @('-display','none','-monitor','tcp:127.0.0.1:4470,server,nowait')
} else {
    $qargs += @('-display','gtk,zoom-to-fit=on')
}

# 地狱模式直接写盘（不走 snapshot），用来验证掉电/重启后的日志重放。
if (-not $Persist -and -not $Hell) {
    $qargs += '-snapshot'
}
if ($NoReboot) {
    $qargs += '-no-reboot'
}

$modeText = 'snapshot (changes discarded)'
if ($Persist) { $modeText = 'persistent' }
if ($Hell) {
    $modeText = "hell / persistent {0}GB sparse disk" -f $HellDiskGB
}

Write-Host ("Image  : {0}" -f $Image)
Write-Host ("Memory : {0} MB" -f $MemoryMB)
Write-Host ("Serial : {0}" -f $serialLog)
Write-Host ("Mode   : {0}" -f $modeText)
Write-Host 'GUI: after login run  bootmode gui  then  reboot.'
Write-Host 'Mouse: click the window to capture, Ctrl+Alt+G releases.'

& $qemu @qargs
