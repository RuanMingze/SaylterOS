$bld = 'build'
if (!(Test-Path $bld)) { New-Item -ItemType Directory -Path $bld | Out-Null }

# ============================================================
# 编译参数（x86_64 / 长模式 / 无运行时）
# ------------------------------------------------------------
# -mno-red-zone      : 中断与任务切换会覆盖红区，必须禁用
# -mno-sse* : 裸机下 CR4.OSFXSR 未置位，执行 SSE 指令会触发 #UD，
#             表现为命令执行到一半卡死。x87 保留可用（CR0.EM/TS 均为 0），
#             slyzfs 的大小格式化用 double，需要 x87 而不是 libgcc 软浮点。
# -mno-stack-arg-probe : 避免引入 __chkstk_ms
# -fno-stack-protector  : 避免引入 __stack_chk_fail
# -mcmodel=small     : 所有地址 < 2GB，使用 32 位绝对寻址
# ============================================================
$cf = '-ffreestanding -m64 -nostdlib -nostartfiles -fno-builtin -O0 ' +
      '-Isrc\include ' +
      '-fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables ' +
      '-fno-stack-protector -mno-red-zone -mno-stack-arg-probe ' +
      '-mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mcmodel=small'
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

$SECTOR_BOOT   = 0
$SECTOR_STAGE2 = 1
$SECTOR_PM     = 5
$SECTOR_KERNEL = 19
$SECTOR_FS     = 8192
$IDE_SECTORS   = 65536

$KERNEL_BASE   = 0x100000     # linker.ld 的加载地址（1MB，64KB 对齐）
$LOAD_END_MAX  = 0x3F0000     # 0x3FF000 是内核初始栈顶，镜像不能顶到栈
$BSS_END_MAX   = 0x01400000   # BSS 从 18MB 起，必须留在 20MB 内存范围内
$PM_MAX_BYTES  = 0x1600       # 0x9C00 - 0x8600，VBE 实模式缓冲区起点

function Write-Sector($src, $dstImg, $lba) {
    & $dd "if=$src" "of=$dstImg" "bs=512" "seek=$lba" "conv=notrunc,sparse,fsync" 2>$null
}

function Build-IdeImg($outPath, $pmBin, $kernelBin) {
    Remove-Item $outPath -ErrorAction SilentlyContinue
    & fsutil file createnew $outPath ($IDE_SECTORS * 512) 2>$null | Out-Null
    & fsutil sparse setflag $outPath 2>$null | Out-Null
    Write-Sector "$bld\boot.bin"   $outPath $SECTOR_BOOT
    Write-Sector "$bld\stage2.bin" $outPath $SECTOR_STAGE2
    Write-Sector $pmBin             $outPath $SECTOR_PM
    Write-Sector $kernelBin         $outPath $SECTOR_KERNEL
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

foreach ($f in @('keyboard','mouse','font16x32','reboot','acpi','sha256','auth','slyzvm','mem','task','bsod','net','netcmds','system_cfg','lang','apic','smp','vm','unicode','ime','ime_pinyin','ime_words','font_cjk','slys')) {
    $out = & $gcc $cfArgs -c -o "$bld\$f.o" "src\kernel\$f.c" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  COMPILE FAILED: $f" -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
}
foreach ($f in @('tty','editor','ui','html','image','audio')) {
    $out = & $gcc $cfFastArgs -c -o "$bld\$f.o" "src\kernel\$f.c" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  COMPILE FAILED: $f" -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
}
foreach ($f in @('kernel','shell')) {
    $out = & $gcc $cfVbeArgs -c -o "$bld\$f.o" "src\kernel\$f.c" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  COMPILE FAILED: $f" -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
}
$out = & $gcc $cfVbeFastArgs -c -o "$bld\vbe.o" "src\kernel\vbe.c" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host '  COMPILE FAILED: vbe' -ForegroundColor Red
    $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
    exit 1
}
# gui.c 是整套图形会话（启动页/登录/桌面/文件管理器/编辑器），
# 绘制循环在 -O2 下才不至于让打字与窗口拖动明显掉帧。
$out = & $gcc $cfVbeFastArgs -c -o "$bld\gui.o" "src\kernel\gui.c" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host '  COMPILE FAILED: gui' -ForegroundColor Red
    $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
    exit 1
}

# SLYZ 应用宿主：每帧执行虚拟机指令并绘制窗口内容区，
# 同样放在绘制热路径，使用 -O2 编译。
$out = & $gcc $cfVbeFastArgs -c -o "$bld\gui_slyz.o" "src\kernel\gui_slyz.c" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host '  COMPILE FAILED: gui_slyz' -ForegroundColor Red
    $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
    exit 1
}

& $gpp $cfArgs -DSLYZFS_NO_STDIO -fno-exceptions -fno-rtti -c -o "$bld\slyzfs.o" "src\fs\slyzfs.cpp" 2>&1
& $gpp $cfArgs -DSLYZFS_NO_STDIO -fno-exceptions -fno-rtti -c -o "$bld\slyzfs_bridge.o" "src\fs\slyzfs_bridge.cpp" 2>&1
& $gpp $cfArgs -DSLYZFS_NO_STDIO -fno-exceptions -fno-rtti -c -o "$bld\kernel_disk.o" "src\fs\kernel_disk.cpp" 2>&1

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
& $gcc $acpicaArgs -c -o "$bld\acpica_glue.o" "src\kernel\acpica_glue.c" 2>&1
if (!(Test-Path "$bld\acpica_osl.o") -or !(Test-Path "$bld\acpica_glue.o")) {
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

foreach ($o in @('entry64','tty','keyboard','mouse','vbe','shell','font16x32','kernel','reboot','editor','ui','html','image','audio','slys','acpi','sha256','auth','slyzvm','mem','task','bsod','net','netcmds','system_cfg','lang','apic','smp','vm','gui','gui_slyz','unicode','ime','ime_pinyin','ime_words','font_cjk','slyzfs','slyzfs_bridge','kernel_disk')) {
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
    "$bld\vbe.o" "$bld\shell.o" "$bld\font16x32.o" "$bld\kernel.o" "$bld\reboot.o" "$bld\editor.o" "$bld\ui.o" "$bld\html.o" "$bld\image.o" "$bld\audio.o" "$bld\slys.o" "$bld\gui.o" "$bld\gui_slyz.o" "$bld\acpi.o" `
    "$bld\apic.o" "$bld\smp.o" "$bld\ap_tramp.o" `
    "$bld\sha256.o" "$bld\auth.o" "$bld\slyzvm.o" "$bld\mem.o" "$bld\task.o" "$bld\bsod.o" "$bld\vm.o" `
    "$bld\unicode.o" "$bld\ime.o" "$bld\ime_pinyin.o" "$bld\ime_words.o" "$bld\font_cjk.o" `
    "$bld\net.o" "$bld\netcmds.o" "$bld\system_cfg.o" "$bld\lang.o" `
    "$bld\slyzfs.o" "$bld\slyzfs_bridge.o" "$bld\kernel_disk.o" `
    "$bld\acpica_osl.o" "$bld\acpica_glue.o" $acpicaObjs `
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
& $nasm -f bin "-DKERNEL_SECTORS=$kernelSectors" -o "$bld\pm.bin" "src\boot\pm.asm" 2>&1
if (!(Test-Path "$bld\pm.bin")) {
    Write-Host '  PM BUILD FAILED' -ForegroundColor Red
    exit 1
}
$pmSize = (Get-Item "$bld\pm.bin").Length
if ($pmSize -gt $PM_MAX_BYTES) {
    Write-Host ("  PM TOO LARGE: {0}B > {1}B (would overwrite VBE buffers at 0x9C00)" -f $pmSize, $PM_MAX_BYTES) -ForegroundColor Red
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
    'tools\slyzc\slyzrt_host.c',
    'src\kernel\slyzvm.c'
)
& $gcc -std=c11 -O2 -Wall -Wextra -DSLYZ_HOST '-Isrc\include' '-Isrc\kernel' `
    -o "$bld\slyzc.exe" @slyzSources
if ($LASTEXITCODE -ne 0) {
    Write-Host '  SLYZC BUILD FAILED' -ForegroundColor Red
    exit 1
}
Write-Host ("  OK slyzc.exe ({0} bytes)" -f (Get-Item "$bld\slyzc.exe").Length) -ForegroundColor Green

# ---------- 编译示例程序 ----------
# 源码 .slyzcode -> 产物 .slyz（SLYZBIN v2 字节码），产物放入系统盘
# 的 system\apps，启动后可以直接 run。
New-Item -ItemType Directory -Force -Path "$bld\apps" | Out-Null
foreach ($app in @('hello', 'textstats', 'gui-window', 'calc', 'settings', 'taskmgr', 'files', 'media')) {
    & "$bld\slyzc.exe" compile "samples\$app.slyzcode" -o "$bld\apps\$app.slyz"
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  SLYZC FAILED: {0}" -f $app) -ForegroundColor Red
        exit 1
    }
}

# ---------- 预置资源（壁纸 / 头像） ----------
# 系统盘里的图片必须真实存在，设置应用才能列出并切换。
# mkasset 在构建期生成 24 位 BMP，和代码一起版本化，避免仓库里
# 提交大体积二进制素材。
& $gpp -std=c++17 -O2 -o "$bld\mkasset.exe" 'tools\mkasset\mkasset.cpp'
if ($LASTEXITCODE -ne 0) {
    Write-Host '  MKASSET BUILD FAILED' -ForegroundColor Red
    exit 1
}
& "$bld\mkasset.exe" "$bld\assets"
if ($LASTEXITCODE -ne 0) {
    Write-Host '  MKASSET RUN FAILED' -ForegroundColor Red
    exit 1
}

# ---------- 预置工具 ----------
& $gpp -std=c++17 -O2 '-Isrc\fs' -o "$bld\mkfs.exe" 'tools\mkfs\mkfs.cpp' 'src\fs\slyzfs.cpp'
if ($LASTEXITCODE -ne 0) {
    Write-Host '  MKFS BUILD FAILED' -ForegroundColor Red
    exit 1
}

Write-Host '===== driver (.slys) ====='

# 音频驱动独立链接到固定地址 0x02000000，内核只保留影子层。
# 驱动不链接内核符号，所有服务通过 SlysKernelApi 获取。
$drvBld = "$bld\driver"
if (!(Test-Path $drvBld)) { New-Item -ItemType Directory -Path $drvBld | Out-Null }

$drvCf = $cf + ' -O2 -Isrc\kernel -fno-tree-loop-distribute-patterns'
$drvArgs = $drvCf.Split(' ')
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
function Get-DriverSym($lines, $name) {
    $m = @($lines | Where-Object { $_ -match ("\s" + $name + "\s*$") })
    if ($m.Count -eq 0) { return [uint64]0 }
    return [Convert]::ToUInt64((($m[0] -split '\s+') | Where-Object { $_ -ne '' })[0], 16)
}
$drvEntry = Get-DriverSym $nmDrv 'slys_driver_entry'
$drvBssStart = Get-DriverSym $nmDrv '__bss_start'
$drvBssEnd = Get-DriverSym $nmDrv '__bss_end'
if ($drvEntry -eq 0 -or $drvBssEnd -lt $drvBssStart) {
    Write-Host '  DRIVER SYMBOL CHECK FAILED' -ForegroundColor Red
    exit 1
}
$drvEntryOff = [uint32]($drvEntry - 0x02000000)
$drvBssSize = [uint32]($drvBssEnd - $drvBssStart)

& $gpp -std=c++17 -O2 -o "$bld\mkslys.exe" 'tools\mkslys\mkslys.cpp'
if ($LASTEXITCODE -ne 0) {
    Write-Host '  MKSLYS BUILD FAILED' -ForegroundColor Red
    exit 1
}
& "$bld\mkslys.exe" "$drvBld\audio.bin" "$bld\audio.slys" "audio" "0x02000000" `
    ("0x{0:X}" -f $drvEntryOff) ("0x{0:X}" -f $drvBssSize)
if ($LASTEXITCODE -ne 0 -or !(Test-Path "$bld\audio.slys")) {
    Write-Host '  MKSLYS FAILED' -ForegroundColor Red
    exit 1
}

Write-Host '===== images ====='

# ---------- 正式系统盘（IDE） ----------
Build-IdeImg "$bld\saylter-ide.img" "$bld\pm.bin" "$bld\kernel.bin"

# 把编译好的 SLYZ 程序写进系统盘。mkfs 在镜像里没有有效文件系统时
# 会先格式化，所以这一步同时承担"首次建立 system\apps 目录"的职责。
$preload = @(
    "$bld\apps\hello.slyz=system\apps\hello.slyz",
    "$bld\apps\textstats.slyz=system\apps\textstats.slyz",
    "$bld\apps\gui-window.slyz=system\apps\gui-window.slyz",
    "$bld\apps\calc.slyz=system\apps\calc.slyz",
    "$bld\apps\settings.slyz=system\apps\settings.slyz",
    "$bld\apps\taskmgr.slyz=system\apps\taskmgr.slyz",
    "$bld\apps\files.slyz=system\apps\files.slyz",
    "$bld\apps\media.slyz=system\apps\media.slyz",
    "$bld\audio.slys=system\drivers\audio.slys",
    "assets\music\周杰伦-牛仔很忙.mp3=home\root\周杰伦-牛仔很忙.mp3",
    "$bld\assets\wallpapers\aurora.bmp=system\wallpapers\aurora.bmp",
    "$bld\assets\wallpapers\dusk.bmp=system\wallpapers\dusk.bmp",
    "$bld\assets\wallpapers\forest.bmp=system\wallpapers\forest.bmp",
    "$bld\assets\wallpapers\midnight.bmp=system\wallpapers\midnight.bmp",
    "$bld\assets\avatars\aurora.bmp=system\avatars\aurora.bmp",
    "$bld\assets\avatars\ember.bmp=system\avatars\ember.bmp",
    "$bld\assets\avatars\leaf.bmp=system\avatars\leaf.bmp",
    "$bld\assets\avatars\slate.bmp=system\avatars\slate.bmp"
)
$mkfsLog = & "$bld\mkfs.exe" "$bld\saylter-ide.img" @preload 2>&1
if ($LASTEXITCODE -ne 0) {
    $mkfsLog | Select-Object -Last 20
    Write-Host '  SLYZ PRELOAD FAILED' -ForegroundColor Red
    exit 1
}
Write-Host '  OK system\apps\*.slyz preloaded' -ForegroundColor Green

$f = Get-Item "$bld\saylter-ide.img"
Write-Host "  OK saylter-ide.img (release image, logical $($f.Length)B / sparse, FS LBA$SECTOR_FS)" -ForegroundColor Green

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
$PAYLOAD_SECTORS        = 19 + $kernelSectors
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
Remove-Item $installerImg -ErrorAction SilentlyContinue
& fsutil file createnew $installerImg ($INSTALLER_SECTORS * 512) 2>$null | Out-Null
Write-Sector "$bld\installer_boot.bin" $installerImg 0
Write-Sector "$bld\installer_main.bin" $installerImg 1
Write-Sector "$bld\boot.bin"           $installerImg ($PAYLOAD_LBA + $SECTOR_BOOT)
Write-Sector "$bld\stage2.bin"         $installerImg ($PAYLOAD_LBA + $SECTOR_STAGE2)
Write-Sector "$bld\pm.bin"             $installerImg ($PAYLOAD_LBA + $SECTOR_PM)
Write-Sector "$bld\kernel.bin"         $installerImg ($PAYLOAD_LBA + $SECTOR_KERNEL)

$fi = Get-Item $installerImg
Write-Host ("  OK saylter-installer.img ({0}B / 16MB HD image, payload LBA{1}, {2} sectors)" -f $fi.Length, $PAYLOAD_LBA, $PAYLOAD_SECTORS) -ForegroundColor Green
