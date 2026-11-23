param(
    [switch]$Debug,
    [int]$ImageSizeGB = 0,
    [string]$ImagePath = ''
)

$bld = 'build'
if (!(Test-Path $bld)) { New-Item -ItemType Directory -Path $bld | Out-Null }

# ============================================================
# 编译参数（x86_64 / 长模式 / 无运行时）
# ------------------------------------------------------------
# -mno-red-zone      : 中断与任务切换会覆盖红区，必须禁用
# -mno-stack-arg-probe : 避免引入 __chkstk_ms
# -fno-stack-protector  : 避免引入 __stack_chk_fail
# -mcmodel=small     : 所有地址 < 2GB，使用 32 位绝对寻址
#
# -mno-sse*：QEMU TCG 逐条翻译 SIMD 指令，SSE2 编译的热路径在
# 虚拟机上实测反而更慢（显示与解码都是如此）。真机上 SSE2 有收益，
# 但在得到可对比的帧时间数据之前不再全局启用。pm.asm 仍然设置
# CR0.MP 与 CR4.OSFXSR/OSXMMEXCPT，为将来按 CPU 能力分组编译留好
# 硬件前提；当前没有代码生成 SSE 指令。
# ============================================================
$dbgDefine = ''
if ($Debug) {
    $dbgDefine = ' -DSAYLTER_DEBUG=1'
    Write-Host '===== DEBUG MODE =====' -ForegroundColor Yellow
    Write-Host '  early console + serial dump + HUD + safe format' -ForegroundColor Yellow
}

$cf = '-ffreestanding -m64 -nostdlib -nostartfiles -fno-builtin -O0 ' +
      '-Isrc\include ' +
      '-fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables ' +
      '-fno-stack-protector -mno-red-zone -mno-stack-arg-probe ' +
      '-mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mcmodel=small' + $dbgDefine
$cfVbe = $cf + ' -DSAYLTER_VBE=1'
$cfArgs = $cf.Split(' ')
$cfVbeArgs = $cfVbe.Split(' ')

# 绘制热路径用 -O2：tty/vbe/editor 的像素循环在 -O0 下慢一个数量级。
# -fno-tree-loop-distribute-patterns 防止循环被改写成 memcpy/memset 调用。
$cfFast = $cf + ' -O2 -fno-tree-loop-distribute-patterns'
$cfVbeFast = $cfVbe + ' -O2 -fno-tree-loop-distribute-patterns'
$cfFastArgs = $cfFast.Split(' ')
$cfVbeFastArgs = $cfVbeFast.Split(' ')

$gcc = 'D:\msys2-64\ucrt64\bin\gcc.exe'
$gpp = 'D:\msys2-64\ucrt64\bin\g++.exe'
$nasm = 'D:\msys2-64\mingw64\bin\nasm.exe'
$objcopy = 'D:\msys2-64\ucrt64\bin\objcopy.exe'
$nm = 'D:\msys2-64\ucrt64\bin\nm.exe'
$size = 'D:\msys2-64\ucrt64\bin\size.exe'
$dd = 'D:\msys2-64\usr\bin\dd.exe'

# g++/gcc 生成的构建期工具（slyzc / mkasset / mkfs2 / mkslys）动态链接
# ucrt64 运行库。若 PATH 里先命中 mingw64 或 mingw32 的同名 DLL，进程会
# 以 0xC0000139（ENTRYPOINT_NOT_FOUND）退出，表现为"工具明明编译成功却
# 跑不起来"。这里把 ucrt64\bin 固定放到 PATH 最前面。
$ucrtBin = 'D:\msys2-64\ucrt64\bin'
if ((Test-Path $ucrtBin) -and (($env:PATH -split ';') -notcontains $ucrtBin)) {
    $env:PATH = "$ucrtBin;$env:PATH"
}

$SECTOR_BOOT   = 0
$SECTOR_STAGE2 = 1
$SECTOR_PM     = 5
$SECTOR_KERNEL = 19
$SECTOR_MODULE = 6144      # SlyzFS v2 .slys 原始模块（内核载荷之后）
$SECTOR_FS     = 8192
# 默认 128MB；-ImageSizeGB 可生成稀疏大镜像（1GB = 2097152 个 512B 扇区）。
$IDE_SECTORS   = 262144
if ($ImageSizeGB -gt 0) {
    $IDE_SECTORS = [int64]$ImageSizeGB * 2097152
}
$ideImg = if ($ImagePath -ne '') { $ImagePath } else { "$bld\saylter-ide.img" }

$KERNEL_BASE   = 0x100000     # linker.ld 的加载地址（1MB，64KB 对齐）
$LOAD_END_MAX  = 0x3F0000     # 0x3FF000 是内核初始栈顶，镜像不能顶到栈
$BSS_END_MAX   = 0x01800000   # BSS 从 18MB 起，驱动固定在 32MB，留 8MB 间隔
$PM_MAX_BYTES  = 0x1600       # 0x9C00 - 0x8600，VBE 实模式缓冲区起点

function Write-Sector($src, $dstImg, $lba) {
    & $dd "if=$src" "of=$dstImg" "bs=512" "seek=$lba" "conv=notrunc,sparse,fsync" 2>$null
}

# 创建稀疏镜像：先标记 sparse，再 SetLength。
# 这样 512GB 的"地狱盘"在磁盘上只占用实际写入的几十 MB。
function New-SparseImage($path, [int64]$bytes) {
    if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).Length -eq $bytes)) {
        return
    }
    Remove-Item $path -ErrorAction SilentlyContinue
    # 镜像被重建时文件系统区域为空：清掉增量标记，强制 mkfs2 重新格式化。
    $fsStampNow = "$bld\.fsstamp"
    if (Test-Path -LiteralPath $fsStampNow) { Remove-Item -LiteralPath $fsStampNow -Force }
    $fs = [IO.File]::Create($path)
    $fs.Close()
    & fsutil sparse setflag $path 2>$null | Out-Null
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    $fs.SetLength($bytes)
    $fs.Close()
}

function Build-IdeImg($outPath, $pmBin, $kernelBin) {
    New-SparseImage $outPath ([int64]$IDE_SECTORS * 512)
    Write-Sector "$bld\boot.bin"   $outPath $SECTOR_BOOT
    Write-Sector "$bld\stage2.bin" $outPath $SECTOR_STAGE2
    Write-Sector $pmBin             $outPath $SECTOR_PM
    Write-Sector $kernelBin         $outPath $SECTOR_KERNEL
    if (Test-Path "$bld\slyzfs2.slys") {
        Write-Sector "$bld\slyzfs2.slys" $outPath $SECTOR_MODULE
    }
}

Write-Host '===== compile ====='

& $nasm -f bin -o "$bld\stage2.bin" "src\boot\stage2.asm" 2>&1 | Out-Null
& $nasm -f elf64 -o "$bld\entry64.o" "src\boot\entry64.asm" 2>&1
if (!(Test-Path "$bld\entry64.o")) {
    Write-Host '  NASM FAILED (entry64)' -ForegroundColor Red
    exit 1
}

# AP 启动引导：16 位实模式 -> 32 位 -> 64 位，作为内核 .text 的一部分，
# 运行时由 smp.c 复制到 0x7000 再发 SIPI。
& $nasm -f elf64 -o "$bld\ap_tramp.o" "src\boot\ap_tramp.asm" 2>&1
if (!(Test-Path "$bld\ap_tramp.o")) {
    Write-Host '  NASM FAILED (ap_tramp)' -ForegroundColor Red
    exit 1
}
# pm.asm 需要内核扇区数，等 kernel.bin 生成后再汇编（见下方）

$stage2Size = (Get-Item "$bld\stage2.bin").Length
if ($stage2Size -ne 2048) {
    Write-Host ("  STAGE2 SIZE: {0}B (expected 2048B)" -f $stage2Size) -ForegroundColor Red
    exit 1
}

# ---- 调试模式：把 Ubuntu Mono 嵌入内核 .rodata ----
# 早期调试控制台在 SlyzFS 挂载前就需要平滑拉丁字形，因此把字体
# 作为只读资源链接进内核；运行时仍由 SlyerType 解析轮廓。
$fontEmbedObj = @()
if ($Debug) {
    Copy-Item 'fonts\ubuntu-mono\UbuntuMono-Regular.ttf' "$bld\stfont.bin" -Force
    & $objcopy -I binary -O elf64-x86-64 -B i386:x86-64 `
        --rename-section '.data=.rodata,alloc,load,readonly,data,contents' `
        --redefine-sym '_binary_build_stfont_bin_start=slyertype_embedded_ttf' `
        --redefine-sym '_binary_build_stfont_bin_end=slyertype_embedded_ttf_end' `
        "$bld\stfont.bin" "$bld\font_embed.o" 2>&1 | Out-Null
    if (!(Test-Path "$bld\font_embed.o")) {
        Write-Host '  FONT EMBED FAILED' -ForegroundColor Red
        exit 1
    }
    $fontEmbedObj = @("$bld\font_embed.o")
    Write-Host '  embedded Ubuntu Mono for early debug console'
}

# ============================================================
# 增量编译
# ------------------------------------------------------------
# 只有"源文件比 .o 新"或"公共头文件比 .o 新"才重新编译。
# 公共头文件的修改会让所有内核对象失效；build.ps1 自身改动也
# 视为依赖变化（编译参数可能变了）。
# 调试/发布模式共用 build 目录，模式切换时强制全量，避免把
# -DSAYLTER_DEBUG 的对象混进发布镜像。
# ============================================================
$buildMode = if ($Debug) { 'debug' } else { 'release' }
$modeFile = "$bld\.buildmode"
$forceAll = $true
if (Test-Path $modeFile) {
    $oldMode = (Get-Content -LiteralPath $modeFile -Raw -ErrorAction SilentlyContinue)
    if ($oldMode -and $oldMode.Trim() -eq $buildMode) { $forceAll = $false }
}
Set-Content -LiteralPath $modeFile -Value $buildMode -NoNewline

$depStamp = (Get-Item 'build.ps1').LastWriteTime
foreach ($h in @(Get-ChildItem 'src\include' -File -Filter *.h) +
               @(Get-ChildItem 'src\kernel' -File -Filter *.h) +
               @(Get-ChildItem 'src\fs' -File -Filter *.h)) {
    if ($h.LastWriteTime -gt $depStamp) { $depStamp = $h.LastWriteTime }
}

function Need-Compile([string]$obj, [string]$src) {
    if ($script:forceAll) { return $true }
    if (!(Test-Path $obj)) { return $true }
    $o = (Get-Item $obj).LastWriteTime
    if ($o -lt $script:depStamp) { return $true }
    if ((Get-Item $src).LastWriteTime -gt $o) { return $true }
    return $false
}

function Build-CFile([string]$obj, [string]$src, [string[]]$flags, [string]$label) {
    if (!(Need-Compile $obj $src)) { return }
    $out = & $gcc @flags -c -o $obj $src 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  COMPILE FAILED: {0}" -f $label) -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
}

# 构建产物（exe / .slyz / .slys）是否需要重做：产物比全部输入新才跳过。
function Need-BuildFile([string]$out, [string[]]$srcs) {
    if ($script:forceAll) { return $true }
    if (!(Test-Path -LiteralPath $out)) { return $true }
    $o = (Get-Item -LiteralPath $out).LastWriteTime
    if ($o -lt $script:depStamp) { return $true }
    foreach ($s in $srcs) {
        if (!(Test-Path -LiteralPath $s)) { return $true }
        if ((Get-Item -LiteralPath $s).LastWriteTime -gt $o) { return $true }
    }
    return $false
}

foreach ($f in @('keyboard','mouse','font16x32','reboot','acpi','sha256','auth','slyzvm','mem','task','bsod','system_cfg','lang','apic','smp','vm','unicode','ime','ime_pinyin','ime_words','font_cjk','slys','faultlab','nvme')) {
    Build-CFile "$bld\$f.o" "src\kernel\$f.c" $cfArgs $f
}
foreach ($f in @('tty','editor','ui','html','image','inflate','png_dec','jpeg_dec','svg_dec','audio','crypto','x509','tls','net','netcmds','slyzvm3','slyz3_tty','slyzui','gpu','usb_msc','ehci_msc','ahci','blk','slyertype','debug')) {
    Build-CFile "$bld\$f.o" "src\kernel\$f.c" $cfFastArgs $f
}
foreach ($f in @('kernel','shell')) {
    Build-CFile "$bld\$f.o" "src\kernel\$f.c" $cfVbeArgs $f
}
Build-CFile "$bld\vbe.o" "src\kernel\vbe.c" $cfVbeFastArgs 'vbe'
# gui.c 是整套图形会话（启动页/登录/桌面/文件管理器/编辑器），
# 绘制循环在 -O2 下才不至于让打字与窗口拖动明显掉帧。
foreach ($f in @('gui_core','gui_dwm','gui_taskbar','gui_dialog','gui')) {
    Build-CFile "$bld\$f.o" "src\kernel\$f.c" $cfVbeFastArgs $f
}

# SLYZ 应用宿主：每帧执行虚拟机指令并绘制窗口内容区，
# 同样放在绘制热路径，使用 -O2 编译。
Build-CFile "$bld\gui_slyz.o" "src\kernel\gui_slyz.c" $cfVbeFastArgs 'gui_slyz'
Build-CFile "$bld\slyzfs2_bridge.o" "src\fs\slyzfs2_bridge.c" $cfArgs 'slyzfs2_bridge'



# ============================================================
# ACPICA（AML 解释器）
# ------------------------------------------------------------
# -D__SAYLTEROS__ 让 platform/acenv.h 选中 acsaylter.h（项目自有平台头）
# 编译用 -O2：解释器是热路径，且 -O2 往往比 -O0 的产物更小
# ============================================================
$acpicaInc = 'third_party\acpica\source\include'
$acpicaSrcDir = 'third_party\acpica\source\components'
$acpicaCf = $cf + ' -D__SAYLTEROS__ -I' + $acpicaInc + ' -O2'
$acpicaArgs = $acpicaCf.Split(' ')

if (!(Test-Path "$bld\acpica")) { New-Item -ItemType Directory -Path "$bld\acpica" | Out-Null }

& $gcc $acpicaArgs -c -o "$bld\acpica_osl.o" "src\kernel\acpica_osl.c" 2>&1
Build-CFile "$bld\acpica_glue.o" "src\kernel\acpica_glue.c" $acpicaArgs 'acpica_glue'
# I2C HID 触控板驱动需要 ACPICA 头（_CRS/_DSM 解析）
Build-CFile "$bld\i2c_tp.o" "src\kernel\i2c_tp.c" $acpicaArgs 'i2c_tp'
if (!(Test-Path "$bld\acpica_osl.o") -or !(Test-Path "$bld\acpica_glue.o") -or
    !(Test-Path "$bld\i2c_tp.o")) {
    Write-Host '  ACPICA GLUE COMPILE FAILED' -ForegroundColor Red
    exit 1
}

$acpicaBase = (Resolve-Path $acpicaSrcDir).Path
$acpicaObjs = @()
$acpicaFailed = 0
# rsdump.c 引用 rsdumpinfo.c 里的调试用描述表，而后者整体包在
# ACPI_DEBUG_OUTPUT/DISASSEMBLER/DEBUGGER 条件里；未开调试时无法编译。
# 它只服务 debugger 的资源转储，解释器不需要。
$acpicaSkip = @('rsdump.c')
foreach ($src in (Get-ChildItem $acpicaSrcDir -Recurse -Filter *.c)) {
    if ($acpicaSkip -contains $src.Name) { continue }
    $rel = $src.FullName.Substring($acpicaBase.Length + 1)
    $objName = ($rel -replace '[\\/]', '_' -replace '\.c$', '.o')
    $obj = "$bld\acpica\$objName"
    # 增量编译：.o 比源文件新就跳过（改 ACPICA 头文件时请删掉 build\acpica）
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -gt $src.LastWriteTime)) {
        $acpicaObjs += $obj
        continue
    }
    $out = & $gcc $acpicaArgs -c -o $obj $src.FullName 2>&1
    if (!(Test-Path $obj)) {
        Write-Host "  ACPICA COMPILE FAILED: $rel" -ForegroundColor Red
        $out | Select-Object -Last 15 | ForEach-Object { Write-Host "    $_" }
        $acpicaFailed = 1
        break
    }
    $acpicaObjs += $obj
}
if ($acpicaFailed) {
    exit 1
}
Write-Host ("  ACPICA objects: {0}" -f $acpicaObjs.Count)

foreach ($o in @('entry64','tty','keyboard','mouse','vbe','shell','font16x32','kernel','reboot','editor','ui','html','image','audio','crypto','x509','tls','slys','acpi','sha256','auth','slyzvm','slyzvm3','slyzui','mem','task','bsod','net','netcmds','system_cfg','lang','apic','smp','vm','gui','gui_slyz','unicode','ime','ime_pinyin','ime_words','font_cjk','faultlab','slyzfs2_bridge','gpu','usb_msc','ehci_msc','ahci','nvme','blk','slyertype','debug')) {
    if (!(Test-Path "$bld\$o.o")) {
        Write-Host "  COMPILE FAILED: build\$o.o was not produced" -ForegroundColor Red
        exit 1
    }
}

Write-Host '===== link ====='
# --image-base=0：x86_64 PE 默认 image base 是 0x200000000，符号会变成
# ImageBase + VMA（8GB 以上），既超出页表映射范围也无法用 32 位绝对寻址。
# 显式置 0 后符号值与 linker.ld 的 VMA 完全一致（_start = 0x100000）。
& $gpp $cfArgs -o "$bld\kernel.elf" `
    "$bld\entry64.o" "$bld\tty.o" "$bld\keyboard.o" "$bld\mouse.o" `
    "$bld\vbe.o" "$bld\shell.o" "$bld\font16x32.o" "$bld\kernel.o" "$bld\reboot.o" "$bld\editor.o" "$bld\ui.o" "$bld\html.o" "$bld\image.o" "$bld\inflate.o" "$bld\png_dec.o" "$bld\jpeg_dec.o" "$bld\svg_dec.o" "$bld\audio.o" "$bld\crypto.o" "$bld\x509.o" "$bld\tls.o" "$bld\slys.o" "$bld\slyz3_tty.o" "$bld\gui_core.o" "$bld\gui_dwm.o" "$bld\gui_taskbar.o" "$bld\gui_dialog.o" "$bld\gui.o" "$bld\gui_slyz.o" "$bld\acpi.o" `
    "$bld\apic.o" "$bld\smp.o" "$bld\ap_tramp.o" `
    "$bld\sha256.o" "$bld\auth.o" "$bld\slyzvm.o" "$bld\slyzvm3.o" "$bld\slyzui.o" "$bld\mem.o" "$bld\task.o" "$bld\bsod.o" "$bld\vm.o" "$bld\faultlab.o" `
    "$bld\unicode.o" "$bld\ime.o" "$bld\ime_pinyin.o" "$bld\ime_words.o" "$bld\font_cjk.o" "$bld\slyertype.o" "$bld\debug.o" `    "$bld\net.o" "$bld\netcmds.o" "$bld\system_cfg.o" "$bld\lang.o" `
    "$bld\slyzfs2_bridge.o" `
    "$bld\acpica_osl.o" "$bld\acpica_glue.o" "$bld\i2c_tp.o" $acpicaObjs "$bld\gpu.o" "$bld\usb_msc.o" "$bld\ehci_msc.o" "$bld\ahci.o" "$bld\nvme.o" "$bld\blk.o" $fontEmbedObj `
    '-Wl,--image-base=0' -T linker.ld 2>&1 | Select-String -NotMatch 'section below image' | ForEach-Object { Write-Host $_ }
if (!(Test-Path "$bld\kernel.elf")) {
    Write-Host '  LINK FAILED: build\kernel.elf was not produced' -ForegroundColor Red
    exit 1
}

# ---- 入口地址自检：内核必须落在 0x10000 ----
$nmOut = & $nm "$bld\kernel.elf"
$entryLine = @($nmOut | Where-Object { $_ -match '^[0-9A-Fa-f]+\s+T\s+_start\s*$' })
if ($entryLine.Count -eq 0) {
    Write-Host '  ENTRY CHECK FAILED: symbol _start not found' -ForegroundColor Red
    exit 1
}
$entryAddr = [Convert]::ToUInt64((($entryLine[0] -split '\s+') | Where-Object { $_ -ne '' })[0], 16)
if ($entryAddr -ne $KERNEL_BASE) {
    Write-Host ("  ENTRY CHECK FAILED: _start=0x{0:X} != 0x{1:X}" -f $entryAddr, $KERNEL_BASE) -ForegroundColor Red
    exit 1
}

& $objcopy -O binary "$bld\kernel.elf" "$bld\kernel.bin" 2>&1
if (!(Test-Path "$bld\kernel.bin")) {
    Write-Host '  OBJCOPY FAILED: build\kernel.bin was not produced' -ForegroundColor Red
    exit 1
}
$kernelBytes = (Get-Item "$bld\kernel.bin").Length
Write-Host "  kernel.bin = $kernelBytes B"

# ---- 计算引导需要加载的扇区数（kernel.bin 覆盖 .text/.rodata/.data/.reloc） ----
$kernelSectors = [int][math]::Ceiling($kernelBytes / 512)
$loadEnd = $KERNEL_BASE + $kernelSectors * 512
if ($loadEnd -ge $LOAD_END_MAX) {
    Write-Host ("  LOAD RANGE OVERFLOW: end=0x{0:X} >= 0x{1:X}" -f $loadEnd, $LOAD_END_MAX) -ForegroundColor Red
    exit 1
}

# ---- 文件系统起点必须在内核装载上界之后 ----
# mkfs 会在镜像的 SlyzFS 起始 LBA 写入超级块；如果内核载荷延伸到
# 该 LBA，超级块会覆盖内核尾部的 .rodata/.data（历史上表现为启动
# #GP，logo 指针变成像素字节）。这里同时校验 slyzfs.h 与安装器
# 的 FS_HEAD_LBA 是否一致，三者任一漂移都会让构建直接失败。
$fsConstLine = Select-String -Path 'src\fs\slyzfs.h' -Pattern 'SLYZ_FS_OFFSET\s*=\s*(\d+)'
$instFsLine = Select-String -Path 'src\boot\installer.asm' -Pattern 'FS_HEAD_LBA\s+equ\s+(\d+)'
if (!$fsConstLine -or !$instFsLine) {
    Write-Host '  FS OFFSET CHECK FAILED: cannot read SLYZ_FS_OFFSET / FS_HEAD_LBA' -ForegroundColor Red
    exit 1
}
$fsLba = [int]$fsConstLine.Matches[0].Groups[1].Value
$instFsLba = [int]$instFsLine.Matches[0].Groups[1].Value
if ($fsLba -ne $instFsLba) {
    Write-Host ("  FS OFFSET CHECK FAILED: slyzfs.h={0} installer.asm={1}" -f $fsLba, $instFsLba) -ForegroundColor Red
    exit 1
}
if ($loadEnd -gt ($fsLba * 512)) {
    Write-Host ("  FS OVERLAP: kernel loadEnd=0x{0:X} > FS start LBA{1} (0x{2:X})" -f $loadEnd, $fsLba, ($fsLba * 512)) -ForegroundColor Red
    Write-Host '  Move SLYZ_FS_OFFSET (slyzfs.h + installer.asm) above the kernel payload.' -ForegroundColor Red
    exit 1
}

# ---- 调试裸日志预留区：LBA 7168..7423 ----
# pm.asm 的 VBE 阶段槽 0..5 在 LBA7168 起，C 层 dbg 裸写日志在 LBA7296 起。
# 它必须位于内核载荷/模块之后、SlyzFS 之前，否则会覆盖内核或文件系统。
$DBG_RAW_LBA = 7168
$DBG_RAW_SECTORS = 256
if ($loadEnd -gt ($DBG_RAW_LBA * 512)) {
    Write-Host ("  RAW LOG OVERLAP: kernel loadEnd=0x{0:X} > raw log LBA{1}" -f $loadEnd, $DBG_RAW_LBA) -ForegroundColor Red
    exit 1
}
if (($DBG_RAW_LBA + $DBG_RAW_SECTORS) -gt $fsLba) {
    Write-Host ("  RAW LOG OVERLAP: raw log end LBA{0} > FS start LBA{1}" -f ($DBG_RAW_LBA + $DBG_RAW_SECTORS), $fsLba) -ForegroundColor Red
    exit 1
}
$SECTOR_FS = $fsLba

# ---- 校验 BSS 上界 ----
$bssLine = @($nmOut | Where-Object { $_ -match '\s_bss_end\s*$' })
if ($bssLine.Count -eq 0) {
    Write-Host '  BSS CHECK FAILED: _bss_end not found' -ForegroundColor Red
    exit 1
}
$bssEnd = [Convert]::ToUInt64((($bssLine[0] -split '\s+') | Where-Object { $_ -ne '' })[0], 16)
if ($bssEnd -ge $BSS_END_MAX) {
    Write-Host ("  BSS OVERFLOW: _bss_end=0x{0:X} >= 0x{1:X}" -f $bssEnd, $BSS_END_MAX) -ForegroundColor Red
    exit 1
}

$sizeLine = (& $size "$bld\kernel.elf" | Select-Object -Last 1)
Write-Host ("  size: {0}" -f $sizeLine.Trim())
Write-Host ("  kernel sectors={0}  loadEnd=0x{1:X}  bssEnd=0x{2:X}" -f $kernelSectors, $loadEnd, $bssEnd)

# ---- 用实际扇区数汇编 pm.bin（32 位 ATA PIO 搬运内核到 0x100000） ----
$pmAsmArgs = @("-DKERNEL_SECTORS=$kernelSectors")
if ($Debug) {
    $pmAsmArgs += '-DSAYLTER_DEBUG_BOOT=1'
}
& $nasm -f bin @pmAsmArgs -o "$bld\pm.bin" "src\boot\pm.asm" 2>&1
if (!(Test-Path "$bld\pm.bin")) {
    Write-Host '  PM BUILD FAILED' -ForegroundColor Red
    exit 1
}
$pmSize = (Get-Item "$bld\pm.bin").Length
if ($pmSize -gt $PM_MAX_BYTES) {
    Write-Host ("  PM TOO LARGE: {0}B > {1}B (would overwrite VBE buffers at 0x9C00)" -f $pmSize, $PM_MAX_BYTES) -ForegroundColor Red
    exit 1
}
# 调试构建必须真的把 SAYLTER_DEBUG_BOOT 传进 pm.asm：漏传时 pm 只有
# 3342B，落盘/蜂鸣代码整段消失，真机黑屏就完全没有线索。
if ($Debug -and $pmSize -lt 4000) {
    Write-Host ("  DEBUG PM MISSING LOG/BEEP: {0}B (SAYLTER_DEBUG_BOOT not applied)" -f $pmSize) -ForegroundColor Red
    exit 1
}
Write-Host ("  pm.bin = {0}B" -f $pmSize)

# ---- 生成 boot sector（只加载 LBA1..18：stage2 + pm） ----
& $nasm -f bin -o "$bld\boot.bin" "src\boot\boot.asm" 2>&1
if (!(Test-Path "$bld\boot.bin")) {
    Write-Host '  BOOT BUILD FAILED' -ForegroundColor Red
    exit 1
}
$bootSize = (Get-Item "$bld\boot.bin").Length
if ($bootSize -ne 512) {
    Write-Host ("  BOOT SECTOR SIZE: {0}B (expected 512B)" -f $bootSize) -ForegroundColor Red
    exit 1
}

Write-Host '===== slyz ====='

# ---------- SLYZ v2 工具链 ----------
# 编译器、宿主运行时与内核解释器共用 src/kernel/slyzvm.c 的指令实现，
# 因此这里编译出的 slyzc.exe 与裸机执行结果一致。
$slyzSources = @(
    'tools\slyzc\slyzc_main.c',
    'tools\slyzc\slyzc_lex.c',
    'tools\slyzc\slyzc_parse.c',
    'tools\slyzc\slyzc_gen.c',
    'tools\slyzc\slyzc_util.c',
    'tools\slyzc\slyzc_serve.c',
    'tools\slyzc\slyzrt_host.c',
    'src\kernel\slyzvm.c'
)
if (Need-BuildFile "$bld\slyzc.exe" $slyzSources) {
    & $gcc -std=c11 -O2 -Wall -Wextra -DSLYZ_HOST '-Isrc\include' '-Isrc\kernel' `
        -o "$bld\slyzc.exe" @slyzSources
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  SLYZC BUILD FAILED' -ForegroundColor Red
        exit 1
    }
    Write-Host ("  OK slyzc.exe ({0} bytes)" -f (Get-Item "$bld\slyzc.exe").Length) -ForegroundColor Green
}

# ---------- SLYZ v3 编译器（寄存器字节码 + screen/CSS） ----------
if (Need-BuildFile "$bld\slyz3c.exe" @('tools\slyz3c\slyz3c.c')) {
    & $gcc -std=c11 -O2 -Wall -Wextra -DSLYZ_HOST '-Isrc\include' '-Isrc\kernel' `
        -o "$bld\slyz3c.exe" 'tools\slyz3c\slyz3c.c'
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  SLYZ3C BUILD FAILED' -ForegroundColor Red
        exit 1
    }
    Write-Host ("  OK slyz3c.exe ({0} bytes)" -f (Get-Item "$bld\slyz3c.exe").Length) -ForegroundColor Green
}

# ---------- 编译示例程序 ----------
# 源码 .slyzcode -> 产物 .slyz（SLYZBIN v2 字节码），产物放入系统盘
# 的 system\apps，启动后可以直接 run。
#
# 注意：预装应用已全部迁移到 v3，并且 v3 与 v2 共用同一个输出文件名。
# 如果这里继续编译 settings/calc/files/...，v2 产物会先写入、时间戳变新，
# 紧接着 v3 的增量判定会误以为"产物已是最新"而跳过，最终镜像里装回
# 旧版 v2 应用。因此 v2 只保留没有 v3 版本的 sylfault_tty。
New-Item -ItemType Directory -Force -Path "$bld\apps" | Out-Null
foreach ($app in @('sylfault_tty')) {
    $srcApp = "samples\$app.slyzcode"
    $dstApp = "$bld\apps\$app.slyz"
    if (!(Need-BuildFile $dstApp @($srcApp, "$bld\slyzc.exe"))) { continue }
    & "$bld\slyzc.exe" compile $srcApp -o $dstApp
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  SLYZC FAILED: {0}" -f $app) -ForegroundColor Red
        exit 1
    }
}

# 预装应用全部迁移到 Slyz v3：输出仍用原来的 .slyz 名字，
# 桌面按同一路径启动，不区分 v2/v3。
$v3apps = @(
    @('hello3','hello'),
    @('textstats3','textstats'),
    @('guiwin3','gui-window'),
    @('calc3','calc'),
    @('settings3','settings'),
    @('taskmgr3','taskmgr'),
    @('files3','files'),
    @('media3','media')
)
foreach ($pair in $v3apps) {
    $srcApp = "samples\$($pair[0]).slyz3"
    $dstApp = "$bld\apps\$($pair[1]).slyz"
    if (!(Need-BuildFile $dstApp @($srcApp, "$bld\slyz3c.exe"))) { continue }
    & "$bld\slyz3c.exe" compile $srcApp -o $dstApp
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  SLYZ3C FAILED: {0}" -f $pair[0]) -ForegroundColor Red
        exit 1
    }
}

# SylFault 图形版使用 v3：寄存器字节码 + screen/CSS 声明式界面。
if (Need-BuildFile "$bld\apps\sylfault.slyz" @('samples\sylfault3.slyz3', "$bld\slyz3c.exe")) {
    & "$bld\slyz3c.exe" compile "samples\sylfault3.slyz3" -o "$bld\apps\sylfault.slyz"
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  SLYZ3C FAILED: sylfault3' -ForegroundColor Red
        exit 1
    }
}
if (Need-BuildFile "$bld\apps\slyzfs.slyz" @('samples\slyzfs3.slyz3', "$bld\slyz3c.exe")) {
    & "$bld\slyz3c.exe" compile "samples\slyzfs3.slyz3" -o "$bld\apps\slyzfs.slyz"
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  SLYZ3C FAILED: slyzfs3' -ForegroundColor Red
        exit 1
    }
}

# ---------- 预置资源（壁纸 / 头像） ----------
# 系统盘里的图片必须真实存在，设置应用才能列出并切换。
# mkasset 在构建期生成 24 位 BMP，和代码一起版本化，避免仓库里
# 提交大体积二进制素材。
# mkasset 在构建期生成 24 位 BMP，和代码一起版本化，避免仓库里
# 提交大体积二进制素材。输出根目录必须先存在：mkasset 只创建
# wallpapers/avatars 子目录，不会创建 build\assets 本身。
if (!(Test-Path "$bld\assets")) { New-Item -ItemType Directory -Path "$bld\assets" | Out-Null }
if (Need-BuildFile "$bld\mkasset.exe" @('tools\mkasset\mkasset.cpp')) {
    & $gpp -std=c++17 -O2 -o "$bld\mkasset.exe" 'tools\mkasset\mkasset.cpp'
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  MKASSET BUILD FAILED' -ForegroundColor Red
        exit 1
    }
}
# 素材是确定性生成：工具没变且文件齐全时不必重复写 8 张 BMP。
$assetList = @(
    "$bld\assets\wallpapers\aurora.bmp", "$bld\assets\wallpapers\dusk.bmp",
    "$bld\assets\wallpapers\forest.bmp", "$bld\assets\wallpapers\midnight.bmp",
    "$bld\assets\avatars\aurora.bmp", "$bld\assets\avatars\ember.bmp",
    "$bld\assets\avatars\leaf.bmp", "$bld\assets\avatars\slate.bmp"
)
$needAssets = $forceAll
foreach ($a in $assetList) {
    if (!(Test-Path -LiteralPath $a)) { $needAssets = $true; break }
    if ((Get-Item -LiteralPath $a).LastWriteTime -lt (Get-Item "$bld\mkasset.exe").LastWriteTime) {
        $needAssets = $true
        break
    }
}
if ($needAssets) {
    & "$bld\mkasset.exe" "$bld\assets"
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  MKASSET RUN FAILED' -ForegroundColor Red
        exit 1
    }
}

# ---------- 默认壁纸 / 默认头像 ----------
# 仓库里的原始素材（JPEG 壁纸、SVG 头像）由内核解码器直接读取，
# 不再转 BMP：PNG/JPEG/SVG 支持是本次新增的能力，设置页也能从
# 文件系统里选择任意同名格式。
#
# 壁纸进镜像前转成 1600×900 的 24 位 BMP：
#   原始素材是 8256×5504 的 JPEG，内核软件解码在 QEMU 下要 8 秒
#   以上，输入 gui 后看起来像死机。BMP 无压缩，解码只做一次顺序
#   读取，启动与登录页不再有明显停顿。assets 目录保留原图。
$srcWall = 'assets\wallpapers\default.jpg'
$dstWall = "$bld\assets\wallpapers\default.bmp"
if (Test-Path -LiteralPath $srcWall) {
    $wallDir = Split-Path -Parent $dstWall
    if (!(Test-Path $wallDir)) { New-Item -ItemType Directory -Path $wallDir | Out-Null }
    $needWall = $true
    if (Test-Path -LiteralPath $dstWall) {
        Add-Type -AssemblyName System.Drawing
        $dstImg = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $dstWall).Path)
        $needWall = ($dstImg.Width -ne 1600 -or $dstImg.Height -ne 900)
        $dstImg.Dispose()
    }
    if ($needWall) {
        Add-Type -AssemblyName System.Drawing
        $img = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $srcWall).Path)
        $bmp = New-Object System.Drawing.Bitmap(1600, 900,
            [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $gfx = [System.Drawing.Graphics]::FromImage($bmp)
        $gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $gfx.DrawImage($img, 0, 0, 1600, 900)
        $gfx.Dispose()
        $bmp.Save($dstWall, [System.Drawing.Imaging.ImageFormat]::Bmp)
        $bmp.Dispose()
        $img.Dispose()
        Write-Host '  wallpaper preloaded as 1600x900 BMP' -ForegroundColor DarkGray
    }
}
$srcAvatar = 'assets\avatars\default.svg'
$dstAvatar = "$bld\assets\avatars\default.svg"
if (Test-Path -LiteralPath $srcAvatar) {
    $avDir = Split-Path -Parent $dstAvatar
    if (!(Test-Path $avDir)) { New-Item -ItemType Directory -Path $avDir | Out-Null }
    if (!(Test-Path -LiteralPath $dstAvatar) -or
        (Get-Item -LiteralPath $srcAvatar).LastWriteTime -gt (Get-Item -LiteralPath $dstAvatar).LastWriteTime -or
        (Get-Item -LiteralPath $srcAvatar).Length -ne (Get-Item -LiteralPath $dstAvatar).Length) {
        Copy-Item -LiteralPath $srcAvatar -Destination $dstAvatar -Force
    }
}

# ---------- 预置工具 ----------
# mkfs2 在镜像生成之后构建（它依赖 slyzfs2.bin）。

Write-Host '===== driver (.slys) ====='

# 音频驱动独立链接到固定地址 0x02000000，内核只保留影子层。
# 驱动不链接内核符号，所有服务通过 SlysKernelApi 获取。
$drvBld = "$bld\driver"
if (!(Test-Path $drvBld)) { New-Item -ItemType Directory -Path $drvBld | Out-Null }

if (Need-BuildFile "$bld\mkslys.exe" @('tools\mkslys\mkslys.cpp')) {
    & $gpp -std=c++17 -O2 -o "$bld\mkslys.exe" 'tools\mkslys\mkslys.cpp'
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  MKSLYS BUILD FAILED' -ForegroundColor Red
        exit 1
    }
}

$drvCf = $cf + ' -O2 -Isrc\kernel -fno-tree-loop-distribute-patterns'
$drvArgs = $drvCf.Split(' ')
function Get-DriverSym($lines, $name) {
    $m = @($lines | Where-Object { $_ -match ("\s" + $name + "\s*$") })
    if ($m.Count -eq 0) { return [uint64]0 }
    return [Convert]::ToUInt64((($m[0] -split '\s+') | Where-Object { $_ -ne '' })[0], 16)
}
$driverInputs = @('src\driver\audio_drv.c', 'src\kernel\mp3.c', 'linker_drv.ld', "$bld\mkslys.exe")
if (Need-BuildFile "$bld\audio.slys" $driverInputs) {
    foreach ($src in @(@('audio_drv','src\driver\audio_drv.c'),
                       @('mp3','src\kernel\mp3.c'))) {
        $out = & $gcc $drvArgs -c -o "$drvBld\$($src[0]).o" $src[1] 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  DRIVER COMPILE FAILED: $($src[0])" -ForegroundColor Red
            $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
            exit 1
        }
    }

    $out = & $gpp $cfArgs -o "$drvBld\audio.elf" `
        "$drvBld\audio_drv.o" "$drvBld\mp3.o" `
        '-Wl,--image-base=0' -T linker_drv.ld 2>&1 | Select-String -NotMatch 'section below image' | ForEach-Object { Write-Host $_ }
    if (!(Test-Path "$drvBld\audio.elf")) {
        Write-Host '  DRIVER LINK FAILED' -ForegroundColor Red
        exit 1
    }

    & $objcopy -O binary "$drvBld\audio.elf" "$drvBld\audio.bin" 2>&1
    if (!(Test-Path "$drvBld\audio.bin")) {
        Write-Host '  DRIVER OBJCOPY FAILED' -ForegroundColor Red
        exit 1
    }

    $nmDrv = & $nm "$drvBld\audio.elf"
    $drvEntry = Get-DriverSym $nmDrv 'slys_driver_entry'
    $drvBssStart = Get-DriverSym $nmDrv '__bss_start'
    $drvBssEnd = Get-DriverSym $nmDrv '__bss_end'
    if ($drvEntry -eq 0 -or $drvBssEnd -lt $drvBssStart) {
        Write-Host '  DRIVER SYMBOL CHECK FAILED' -ForegroundColor Red
        exit 1
    }
    $drvEntryOff = [uint32]($drvEntry - 0x02000000)
    $drvBssSize = [uint32]($drvBssEnd - $drvBssStart)

    & "$bld\mkslys.exe" "$drvBld\audio.bin" "$bld\audio.slys" "audio" "0x02000000" `
        ("0x{0:X}" -f $drvEntryOff) ("0x{0:X}" -f $drvBssSize)
    if ($LASTEXITCODE -ne 0 -or !(Test-Path "$bld\audio.slys")) {
        Write-Host '  MKSLYS FAILED' -ForegroundColor Red
        exit 1
    }
}

# ---------- SlyzFS v2 模块（Slyz v3 -> NASM -> .slys） ----------
# 文件系统在"还没有文件系统"时就要运行，因此模块按原始 LBA 加载：
# 头部 64B + 镜像，随后由加载器清零 48MB BSS。
Write-Host '===== slyzfs v2 module ====='
if (Need-BuildFile "$bld\slyzfs2.slys" @('src\fs\slyzfs2.slyz3', "$bld\slyz3c.exe", "$bld\mkslys.exe")) {
    $fsAsm = "$bld\slyzfs2.asm"
    $out = & "$bld\slyz3c.exe" compile 'src\fs\slyzfs2.slyz3' -o $fsAsm `
        --baremetal --entry slyzfs_call --org 0x04000000 --heap 50331648 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  SLYZFS MODULE COMPILE FAILED' -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
    $out = & $nasm -f bin -o "$bld\slyzfs2.bin" $fsAsm 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  SLYZFS MODULE ASSEMBLE FAILED' -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
    & "$bld\mkslys.exe" "$bld\slyzfs2.bin" "$bld\slyzfs2.slys" "slyzfs" `
        "0x04000000" "0x0" "50331648"
    if ($LASTEXITCODE -ne 0 -or !(Test-Path "$bld\slyzfs2.slys")) {
        Write-Host '  SLYZFS MODULE PACK FAILED' -ForegroundColor Red
        exit 1
    }
    $fsMod = Get-Item "$bld\slyzfs2.bin"
    Write-Host ("  OK slyzfs2.slys ({0}B image + 64B header, bss 48MB)" -f $fsMod.Length) -ForegroundColor Green
}
$moduleSectors = [int][math]::Ceiling((Get-Item "$bld\slyzfs2.slys").Length / 512)
if (($SECTOR_KERNEL * 512 + $kernelBytes) -gt ($SECTOR_MODULE * 512)) {
    Write-Host '  MODULE OVERLAPS KERNEL: move SECTOR_MODULE higher' -ForegroundColor Red
    exit 1
}
if (($SECTOR_MODULE + $moduleSectors) -gt $DBG_RAW_LBA) {
    Write-Host ("  MODULE OVERLAPS RAW LOG: module end LBA{0} > {1}" -f ($SECTOR_MODULE + $moduleSectors), $DBG_RAW_LBA) -ForegroundColor Red
    exit 1
}

# ---------- VizCormin：独立预装代码编辑器 ----------
# 编辑器不是内核组件：它被编译成固定基址 0x08000000 的 .slys，
# 只通过 SlysKernelApi 使用分配器、SlyzFS 与 VBE。内核镜像里
# 不含任何编辑器代码，卸载/替换模块不需要重新链接内核。
Write-Host '===== vizcormin (.slys app) ====='
$vcBld = "$bld\vizcormin"
if (!(Test-Path $vcBld)) { New-Item -ItemType Directory -Path $vcBld | Out-Null }

$vcCf = $cf + ' -O2 -DVC_MODULE' +
        ' -Isrc\apps\vizcormin\core -Isrc\apps\vizcormin\app -Isrc\apps\vizcormin\module' +
        ' -fno-tree-loop-distribute-patterns'
$vcCfArgs = $vcCf.Split(' ')
function Get-VcSym($lines, $name) {
    $m = @($lines | Where-Object { $_ -match ("\s" + $name + "\s*$") })
    if ($m.Count -eq 0) { return [uint64]0 }
    return [Convert]::ToUInt64((($m[0] -split '\s+') | Where-Object { $_ -ne '' })[0], 16)
}
$vcSources = @(
    'src\apps\vizcormin\core\vc_alloc.c',
    'src\apps\vizcormin\core\vc_text.c',
    'src\apps\vizcormin\core\vc_view.c',
    'src\apps\vizcormin\core\vc_atlas.c',
    'src\apps\vizcormin\core\vc_render.c',
    'src\apps\vizcormin\core\vc_session.c',
    'src\apps\vizcormin\core\vc_find.c',
    'src\apps\vizcormin\core\vc_command.c',
    'src\apps\vizcormin\core\vc_runtime.c',
    'src\apps\vizcormin\app\vc_app.c',
    'src\apps\vizcormin\module\vc_fs_api.c',
    'src\apps\vizcormin\module\vc_surface_api.c',
    'src\apps\vizcormin\module\vizcormin_entry.c'
)
$vcInputs = $vcSources + @('linker_vizcormin.ld', "$bld\mkslys.exe")
if (Need-BuildFile "$bld\vizcormin.slys" $vcInputs) {
    $vcObjs = @()
    foreach ($src in $vcSources) {
        $obj = "$vcBld\" + ((Split-Path $src -Leaf) -replace '\.c$', '.o')
        $out = & $gcc $vcCfArgs -c -o $obj $src 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  VIZCORMIN COMPILE FAILED: $src" -ForegroundColor Red
            $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
            exit 1
        }
        $vcObjs += $obj
    }

    $out = & $gpp $cfArgs -o "$vcBld\vizcormin.elf" @vcObjs `
        '-Wl,--image-base=0' -T linker_vizcormin.ld 2>&1 |
        Select-String -NotMatch 'section below image' | ForEach-Object { Write-Host $_ }
    if (!(Test-Path "$vcBld\vizcormin.elf")) {
        Write-Host '  VIZCORMIN LINK FAILED' -ForegroundColor Red
        exit 1
    }

    & $objcopy -O binary "$vcBld\vizcormin.elf" "$vcBld\vizcormin.bin" 2>&1
    if (!(Test-Path "$vcBld\vizcormin.bin")) {
        Write-Host '  VIZCORMIN OBJCOPY FAILED' -ForegroundColor Red
        exit 1
    }

    $nmVc = & $nm "$vcBld\vizcormin.elf"
    $vcBase = [uint64]0x08000000
    $vcEntry = Get-VcSym $nmVc 'vizcormin_entry'
    $vcBssStart = Get-VcSym $nmVc '__bss_start'
    $vcBssEnd = Get-VcSym $nmVc '__bss_end'
    if ($vcEntry -eq 0 -or $vcBssEnd -lt $vcBssStart -or $vcEntry -lt $vcBase) {
        Write-Host '  VIZCORMIN SYMBOL CHECK FAILED' -ForegroundColor Red
        exit 1
    }
    $vcEntryOff = [uint32]($vcEntry - $vcBase)
    $vcBssSize = [uint32]($vcBssEnd - $vcBssStart)

    & "$bld\mkslys.exe" "$vcBld\vizcormin.bin" "$bld\vizcormin.slys" "vizcormin" "0x08000000" `
        ("0x{0:X}" -f $vcEntryOff) ("0x{0:X}" -f $vcBssSize)
    if ($LASTEXITCODE -ne 0 -or !(Test-Path "$bld\vizcormin.slys")) {
        Write-Host '  VIZCORMIN PACK FAILED' -ForegroundColor Red
        exit 1
    }
    $vcImg = Get-Item "$vcBld\vizcormin.bin"
    Write-Host ("  OK vizcormin.slys ({0}B image, entry+0x{1:X}, bss {2}B)" -f
                $vcImg.Length, $vcEntryOff, $vcBssSize) -ForegroundColor Green
}

# ---------- 动态链接测试模块 ----------
# libmath 导出 slyz_add，demo 导入并调用，验证 .slyzd 的
# 依赖解析、导入槽回填与固定基址加载。
Write-Host '===== dynamic link demo ====='
if (Need-BuildFile "$bld\mkslyzd.exe" @('tools\mkslyzd\mkslyzd.cpp')) {
    & $gpp -std=c++17 -O2 '-Isrc\include' -o "$bld\mkslyzd.exe" 'tools\mkslyzd\mkslyzd.cpp'
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  MKSLYZD BUILD FAILED' -ForegroundColor Red
        exit 1
    }
}
foreach ($m in @(@('libmath','tools\linkdemo\libmath.asm'),
                 @('demo','tools\linkdemo\demo.asm'))) {
    $asm = $m[1]
    $manifest = "tools\linkdemo\$($m[0]).manifest"
    $dst = "$bld\$($m[0]).slyzd"
    if (!(Need-BuildFile $dst @($asm, $manifest, "$bld\mkslyzd.exe"))) { continue }
    $out = & $nasm -f bin -o "$bld\$($m[0]).bin" $asm 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  LINK DEMO ASSEMBLE FAILED: $($m[0])" -ForegroundColor Red
        $out | Select-Object -Last 10 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
    & "$bld\mkslyzd.exe" "$bld\$($m[0]).bin" $manifest $dst
    if ($LASTEXITCODE -ne 0 -or !(Test-Path $dst)) {
        Write-Host "  MKSLYZD FAILED: $($m[0])" -ForegroundColor Red
        exit 1
    }
    Write-Host ("  OK {0}.slyzd" -f $m[0]) -ForegroundColor Green
}

# ---------- musl 静态应用 ----------
# hello.slys 是 musl 静态链接的验收样例：printf / malloc / exit 全链路。
# musl 源码与适配层都在本仓库（third_party\musl、src\slyzlibc）。
# 交叉工具链缺失时只跳过这一步，内核本身仍然可以构建。
Write-Host '===== musl app ====='
$x64gcc = 'E:\dev\x86_64-elf-tools-windows\bin\x86_64-elf-gcc.exe'
$muslBuild = Join-Path $PSScriptRoot 'tools\build-musl.ps1'
$muslAppOk = $false
if ((Test-Path $x64gcc) -and (Test-Path $muslBuild)) {
    & $muslBuild -App hello -OutDir "$bld"
    if (Test-Path "$bld\hello.slys") {
        $muslAppOk = $true
        Write-Host ("  OK hello.slys ({0}B)" -f
                    (Get-Item "$bld\hello.slys").Length) -ForegroundColor Green
    } else {
        Write-Host '  MUSL HELLO BUILD FAILED' -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host '  SKIP: x86_64-elf 工具链或 Slyztrix 不存在' -ForegroundColor Yellow
}

Write-Host '===== images ====='

# ---------- 正式系统盘（IDE） ----------
Build-IdeImg $ideImg "$bld\pm.bin" "$bld\kernel.bin"

# 把编译好的程序与资源写进 v2 分区（C: / R: / D:）。
# mkfs2 加载同一个 .slys 模块来格式化与写入，保证构建期与运行期
# 使用完全相同的文件系统实现。
$preload = @(
    "$bld\apps\hello.slyz=C:\system\apps\hello.slyz",
    "$bld\apps\textstats.slyz=C:\system\apps\textstats.slyz",
    "$bld\apps\gui-window.slyz=C:\system\apps\gui-window.slyz",
    "$bld\apps\calc.slyz=C:\system\apps\calc.slyz",
    "$bld\apps\settings.slyz=C:\system\apps\settings.slyz",
    "$bld\apps\taskmgr.slyz=C:\system\apps\taskmgr.slyz",
    "$bld\apps\files.slyz=C:\system\apps\files.slyz",
    "$bld\apps\media.slyz=C:\system\apps\media.slyz",
    "$bld\apps\sylfault.slyz=C:\system\apps\sylfault.slyz",
    "$bld\apps\slyzfs.slyz=C:\system\apps\slyzfs.slyz",
    "$bld\apps\sylfault_tty.slyz=C:\system\apps\sylfault-tty.slyz",
    "$bld\audio.slys=C:\system\drivers\audio.slys",
    "$bld\vizcormin.slys=C:\Programs\VizCormin\vizcormin.slys",
    "$bld\libmath.slyzd=C:\system\modules\libmath.slyzd",
    "$bld\demo.slyzd=C:\system\modules\demo.slyzd",
    "fonts\ubuntu-mono\UbuntuMono-Regular.ttf=C:\system\fonts\ubuntu-mono.ttf",
    "fonts\ubuntu-mono\UbuntuMono-Regular.ttf=R:\system\fonts\ubuntu-mono.ttf",
    "fonts\noto-sans-sc\NotoSansSC-VF.ttf=C:\system\fonts\noto-sans-sc.ttf",
    "assets\certs\ca.pem=C:\system\certs\ca.pem",
    # 不预置任何受版权保护的商业音乐；桌面与媒体播放器仍支持用户
    # 自行放入 home\root 的 .mp3，发行镜像里不带第三方音源。
    "$bld\assets\wallpapers\default.bmp=C:\system\wallpapers\default.bmp",
    "$bld\assets\avatars\default.svg=C:\system\avatars\default.svg",
    "$bld\assets\wallpapers\aurora.bmp=C:\system\wallpapers\aurora.bmp",
    "$bld\assets\wallpapers\dusk.bmp=C:\system\wallpapers\dusk.bmp",
    "$bld\assets\wallpapers\forest.bmp=C:\system\wallpapers\forest.bmp",
    "$bld\assets\wallpapers\midnight.bmp=C:\system\wallpapers\midnight.bmp",
    "$bld\assets\avatars\aurora.bmp=C:\system\avatars\aurora.bmp",
    "$bld\assets\avatars\ember.bmp=C:\system\avatars\ember.bmp",
    "$bld\assets\avatars\leaf.bmp=C:\system\avatars\leaf.bmp",
    "$bld\assets\avatars\slate.bmp=C:\system\avatars\slate.bmp"
)
# musl 应用依赖交叉工具链，工具链缺席时不进镜像。
if (Test-Path "$bld\hello.slys") {
    $preload += "$bld\hello.slys=C:\Programs\hello.slys"
}
& $gpp -std=c++17 -O2 '-Isrc\include' -o "$bld\mkfs2.exe" 'tools\mkfs2\mkfs2.cpp'
if ($LASTEXITCODE -ne 0) {
    Write-Host '  MKFS2 BUILD FAILED' -ForegroundColor Red
    exit 1
}

# ---- 文件系统镜像增量判定 ----
# mkfs2 会重新格式化卷并逐个写入预置文件（字体 17MB），是全流程里
# 最慢的一步。预置内容与模块二进制都没变时直接复用现有镜像。
# 用 SHA1 而不是时间戳：mkasset 每次构建都会重写同样的 BMP，
# 时间戳会变但内容不变。
$fsInputs = @()
foreach ($entry in $preload) {
    $fsInputs += (($entry -split '=', 2)[0])
}
$fsInputs += "$bld\slyzfs2.slys"
$fsInputs += 'build.ps1'
$fsParts = @()
foreach ($p in ($fsInputs | Sort-Object -Unique)) {
    if (Test-Path -LiteralPath $p) {
        $fsParts += ($p + ':' + (Get-FileHash -LiteralPath $p -Algorithm SHA1).Hash)
    } else {
        $fsParts += ($p + ':missing')
    }
}
$fsParts += ("image=" + (Resolve-Path -LiteralPath $ideImg -ErrorAction SilentlyContinue) + " sectors=" + $IDE_SECTORS)
$fsText = ($fsParts -join "`n")
$sha1 = [Security.Cryptography.SHA1]::Create()
$fsHash = ([BitConverter]::ToString($sha1.ComputeHash([Text.Encoding]::UTF8.GetBytes($fsText)))).Replace('-', '')
$sha1.Dispose()

$fsStamp = "$bld\.fsstamp"
$fsUpToDate = $false
if (!$forceAll -and (Test-Path -LiteralPath $ideImg) -and (Test-Path -LiteralPath $fsStamp)) {
    $imgLen = (Get-Item -LiteralPath $ideImg).Length
    $oldHash = (Get-Content -LiteralPath $fsStamp -Raw -ErrorAction SilentlyContinue) -replace '\s', ''
    $fsUpToDate = ($imgLen -eq ([int64]$IDE_SECTORS * 512)) -and ($oldHash -eq $fsHash)
}

if ($fsUpToDate) {
    Write-Host '  SlyzFS image up-to-date (format/preload skipped)' -ForegroundColor DarkGray
} else {
    $mkfsLog = & "$bld\mkfs2.exe" $ideImg "$bld\slyzfs2.bin" @preload 2>&1
    if ($LASTEXITCODE -ne 0) {
        # 全量输出：失败条目可能位于列表中段，截断末尾会丢掉真正的错误。
        foreach ($line in $mkfsLog) { Write-Host ("  {0}" -f $line) }
        Write-Host '  SLYZFS V2 PRELOAD FAILED' -ForegroundColor Red
        exit 1
    }
    Set-Content -LiteralPath $fsStamp -Value $fsHash -NoNewline
}
Write-Host '  OK SlyzFS v2 preload (C:/R:)' -ForegroundColor Green

$f = Get-Item $ideImg
Write-Host ("  OK {0} (logical {1}B / sparse, {2} sectors, FS LBA{3})" -f
            $f.Name, $f.Length, $IDE_SECTORS, $SECTOR_FS) -ForegroundColor Green

# ---------- 安装器（16MB 硬盘镜像） ----------
# 布局：
#   LBA0        安装器引导扇区
#   LBA1..16    安装器主体（固定 8192B = 16 扇区）
#   LBA17..     系统 payload：boot.bin + stage2 + pm + kernel
# payload 在安装介质上的排列必须与目标硬盘的 LBA 顺序逐字节一致，
# 安装器只做"介质 LBA17.. -> 硬盘 LBA0.."的原样搬运。
#
# 介质已从 1.44MB 软盘升级为 16MB 硬盘镜像：内核加上中文字库与
# 输入法词表后，2880 扇区已经顶到上限。读盘改用 INT 13h AH=42h
# 的 LBA 扩展读，与 CHS 几何无关；启动时用 -drive if=ide 即可。
$INSTALLER_MAIN_SECTORS = 16
$INSTALLER_MAIN_BYTES   = $INSTALLER_MAIN_SECTORS * 512
$PAYLOAD_LBA            = 1 + $INSTALLER_MAIN_SECTORS
$moduleBytes            = if (Test-Path "$bld\slyzfs2.slys") { (Get-Item "$bld\slyzfs2.slys").Length } else { 0 }
$moduleSectors          = [int][math]::Ceiling($moduleBytes / 512)
$PAYLOAD_MIN            = 19 + $kernelSectors
if ($moduleSectors -gt 0) {
    $PAYLOAD_MIN = [math]::Max($PAYLOAD_MIN, $SECTOR_MODULE + $moduleSectors)
}
$PAYLOAD_SECTORS        = $PAYLOAD_MIN
$INSTALLER_SECTORS      = 32768     # 16MB

if (($PAYLOAD_LBA + $PAYLOAD_SECTORS) -gt $INSTALLER_SECTORS) {
    Write-Host ("  INSTALLER OVERFLOW: needs {0} sectors > {1}" -f ($PAYLOAD_LBA + $PAYLOAD_SECTORS), $INSTALLER_SECTORS) -ForegroundColor Red
    exit 1
}
$installerFree = $INSTALLER_SECTORS - ($PAYLOAD_LBA + $PAYLOAD_SECTORS)
Write-Host ("  installer payload: {0} sectors, free: {1} sectors ({2} KB)" -f ($PAYLOAD_LBA + $PAYLOAD_SECTORS), $installerFree, [int]($installerFree / 2))

& $nasm -f bin "-DINSTALLER_MAIN_SECTORS=$INSTALLER_MAIN_SECTORS" -o "$bld\installer_boot.bin" "src\boot\installer_boot.asm" 2>&1
& $nasm -f bin "-DPAYLOAD_LBA=$PAYLOAD_LBA" "-DPAYLOAD_SECTORS=$PAYLOAD_SECTORS" -o "$bld\installer_main.bin" "src\boot\installer.asm" 2>&1
if (!(Test-Path "$bld\installer_boot.bin") -or !(Test-Path "$bld\installer_main.bin")) {
    Write-Host '  INSTALLER BUILD FAILED' -ForegroundColor Red
    exit 1
}
if ((Get-Item "$bld\installer_boot.bin").Length -ne 512) {
    Write-Host ("  INSTALLER BOOT SIZE: {0}B (expected 512B)" -f (Get-Item "$bld\installer_boot.bin").Length) -ForegroundColor Red
    exit 1
}
if ((Get-Item "$bld\installer_main.bin").Length -ne $INSTALLER_MAIN_BYTES) {
    Write-Host ("  INSTALLER MAIN SIZE: {0}B (expected {1}B)" -f (Get-Item "$bld\installer_main.bin").Length, $INSTALLER_MAIN_BYTES) -ForegroundColor Red
    exit 1
}

$installerImg = "$bld\saylter-installer.img"
$instInputs = @("$bld\installer_boot.bin", "$bld\installer_main.bin",
                "$bld\boot.bin", "$bld\stage2.bin", "$bld\pm.bin",
                "$bld\kernel.bin", "$bld\slyzfs2.slys")
$instNeed = $forceAll -or !(Test-Path -LiteralPath $installerImg)
if (!$instNeed) {
    if ((Get-Item -LiteralPath $installerImg).Length -ne ([int64]$INSTALLER_SECTORS * 512)) {
        $instNeed = $true
    } else {
        $instTime = (Get-Item -LiteralPath $installerImg).LastWriteTime
        foreach ($p in $instInputs) {
            if ((Get-Item -LiteralPath $p).LastWriteTime -gt $instTime) { $instNeed = $true; break }
        }
    }
}
if ($instNeed) {
    Remove-Item $installerImg -ErrorAction SilentlyContinue
    & fsutil file createnew $installerImg ($INSTALLER_SECTORS * 512) 2>$null | Out-Null
    Write-Sector "$bld\installer_boot.bin" $installerImg 0
    Write-Sector "$bld\installer_main.bin" $installerImg 1
    Write-Sector "$bld\boot.bin"           $installerImg ($PAYLOAD_LBA + $SECTOR_BOOT)
    Write-Sector "$bld\stage2.bin"         $installerImg ($PAYLOAD_LBA + $SECTOR_STAGE2)
    Write-Sector "$bld\pm.bin"             $installerImg ($PAYLOAD_LBA + $SECTOR_PM)
    Write-Sector "$bld\kernel.bin"         $installerImg ($PAYLOAD_LBA + $SECTOR_KERNEL)
    if ($moduleSectors -gt 0) {
        Write-Sector "$bld\slyzfs2.slys"   $installerImg ($PAYLOAD_LBA + $SECTOR_MODULE)
    }
}

$fi = Get-Item $installerImg
Write-Host ("  OK saylter-installer.img ({0}B / 16MB HD image, payload LBA{1}, {2} sectors)" -f $fi.Length, $PAYLOAD_LBA, $PAYLOAD_SECTORS) -ForegroundColor Green
