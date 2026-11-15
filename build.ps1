$bld = 'build'
$cf = '-ffreestanding -m32 -nostdlib -nostartfiles -fno-builtin -fno-leading-underscore -O0 -fno-exceptions -fno-asynchronous-unwind-tables -mno-sse -mno-sse2 -mno-mmx -mno-3dnow'
# ^ -mno-sse/-mno-sse2/-mno-mmx 是硬性要求：
#   32 位 MinGW GCC 默认按 pentium4 级别生成代码，会把结构体拷贝等操作编译成
#   movdqu/movups 等 SSE 指令。裸内核里 CR4.OSFXSR 未置位，执行这些指令会触发
#   #UD 无效指令异常，被默认 ISR 吞掉后 iret 到错误地址，表现为"命令执行到一半卡死"。
#   禁用 SSE 后编译器改用整数/rep movsb 实现，x87 浮点仍可用（CR0.EM/TS 均为 0）。
$cfVbe = $cf + ' -DSAYLTER_VBE=1'
$gcc = 'D:\msys2-64\mingw32\bin\gcc.exe'
$gpp = 'D:\msys2-64\mingw32\bin\g++.exe'
$nasm = 'D:\msys2-64\mingw64\bin\nasm.exe'
$objcopy = 'D:\msys2-64\mingw32\bin\objcopy.exe'
$dd = 'D:\msys2-64\usr\bin\dd.exe'

$SECTOR_BOOT   = 0
$SECTOR_STAGE2 = 1
$SECTOR_PM     = 5
$SECTOR_KERNEL = 19
$SECTOR_FS     = 2048
$IDE_SECTORS   = 65536

function Write-Sector($src, $dstImg, $lba) {
    & $dd "if=$src" "of=$dstImg" "bs=512" "seek=$lba" "conv=notrunc,sparse,fsync" 2>$null
}

function Build-FloppyImg($outPath, $pmBin, $kernelBin) {
    Remove-Item $outPath -ErrorAction SilentlyContinue
    Write-Sector "$bld\boot.bin"   $outPath $SECTOR_BOOT
    Write-Sector "$bld\stage2.bin" $outPath $SECTOR_STAGE2
    Write-Sector $pmBin             $outPath $SECTOR_PM
    Write-Sector $kernelBin         $outPath $SECTOR_KERNEL
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

& $nasm -f bin -o "$bld\boot.bin" "src\boot\boot.asm" 2>&1 | Out-Null
& $nasm -f bin -o "$bld\stage2.bin" "src\boot\stage2.asm" 2>&1 | Out-Null
& $nasm -f elf32 -o "$bld\entry32.o" "src\boot\entry32.asm" 2>&1 | Out-Null

Write-Host '===== VBE build ====='
& $nasm -f bin -D SAYLTER_VBE=1 -o "$bld\pm.bin" "src\boot\pm.asm" 2>&1 | Out-Null

foreach ($f in @('tty','keyboard','mouse','font16x32','reboot','editor','acpi','sha256','auth','slyzvm','mem','task')) {
    & $gcc $cf.Split() -c -o "$bld\$f.o" "src\kernel\$f.c" 2>&1
}
foreach ($f in @('kernel','shell','vbe')) {
    & $gcc $cfVbe.Split() -c -o "$bld\$f.o" "src\kernel\$f.c" 2>&1
}

& $gpp ($cf + ' -DSLYZFS_NO_STDIO').Split() -fno-exceptions -fno-rtti -c -o "$bld\slyzfs.o" "src\fs\slyzfs.cpp" 2>&1
& $gpp ($cf + ' -DSLYZFS_NO_STDIO').Split() -fno-exceptions -fno-rtti -c -o "$bld\slyzfs_bridge.o" "src\fs\slyzfs_bridge.cpp" 2>&1
& $gpp ($cf + ' -DSLYZFS_NO_STDIO').Split() -fno-exceptions -fno-rtti -c -o "$bld\kernel_disk.o" "src\fs\kernel_disk.cpp" 2>&1

& $gpp $cf.Split() -o "$bld\kernel.elf" `
    "$bld\entry32.o" "$bld\tty.o" "$bld\keyboard.o" "$bld\mouse.o" `
    "$bld\vbe.o" "$bld\shell.o" "$bld\font16x32.o" "$bld\kernel.o" "$bld\reboot.o" "$bld\editor.o" "$bld\acpi.o" `
    "$bld\sha256.o" "$bld\auth.o" "$bld\slyzvm.o" "$bld\mem.o" "$bld\task.o" `
    "$bld\slyzfs.o" "$bld\slyzfs_bridge.o" "$bld\kernel_disk.o" `
    -T linker.ld 2>&1 | Select-String -NotMatch 'section below image' | Out-Null
if (!(Test-Path "$bld\kernel.elf")) {
    Write-Host '  LINK FAILED: build\kernel.elf was not produced' -ForegroundColor Red
    exit 1
}
& $objcopy -O binary "$bld\kernel.elf" "$bld\kernel.bin" 2>&1
$kSizeVbe = (Get-Item "$bld\kernel.bin").Length
Write-Host "  VBE kernel = ${kSizeVbe}B"

Build-FloppyImg "$bld\saylter.img" "$bld\pm.bin" "$bld\kernel.bin"
Write-Host "  OK saylter.img ($((Get-Item "$bld\saylter.img").Length)B)" -ForegroundColor Green

Write-Host ''
Write-Host '===== TTY build ====='
& $nasm -f bin -o "$bld\pm.bin" "src\boot\pm.asm" 2>&1 | Out-Null

foreach ($f in @('tty','keyboard','kernel','shell','reboot','editor','acpi','sha256','auth','slyzvm','mem','task')) {
    & $gcc $cf.Split() -c -o "$bld\tty2-$f.o" "src\kernel\$f.c" 2>&1
}

& $gpp ($cf + ' -DSLYZFS_NO_STDIO').Split() -fno-exceptions -fno-rtti -c -o "$bld\tty2-slyzfs.o" "src\fs\slyzfs.cpp" 2>&1
& $gpp ($cf + ' -DSLYZFS_NO_STDIO').Split() -fno-exceptions -fno-rtti -c -o "$bld\tty2-slyzfs_bridge.o" "src\fs\slyzfs_bridge.cpp" 2>&1
& $gpp ($cf + ' -DSLYZFS_NO_STDIO').Split() -fno-exceptions -fno-rtti -c -o "$bld\tty2-kernel_disk.o" "src\fs\kernel_disk.cpp" 2>&1

& $gpp $cf.Split() -o "$bld\kernel-tty.elf" `
    "$bld\entry32.o" "$bld\tty2-tty.o" "$bld\tty2-keyboard.o" `
    "$bld\tty2-shell.o" "$bld\tty2-kernel.o" "$bld\tty2-reboot.o" "$bld\tty2-editor.o" "$bld\tty2-acpi.o" `
    "$bld\tty2-sha256.o" "$bld\tty2-auth.o" "$bld\tty2-slyzvm.o" "$bld\tty2-mem.o" "$bld\tty2-task.o" `
    "$bld\tty2-slyzfs.o" "$bld\tty2-slyzfs_bridge.o" "$bld\tty2-kernel_disk.o" `
    -T linker.ld 2>&1 | Select-String -NotMatch 'section below image' | Out-Null
if (!(Test-Path "$bld\kernel-tty.elf")) {
    Write-Host '  LINK FAILED: build\kernel-tty.elf was not produced' -ForegroundColor Red
    exit 1
}
& $objcopy -O binary "$bld\kernel-tty.elf" "$bld\kernel-tty.bin" 2>&1
$kSizeTty = (Get-Item "$bld\kernel-tty.bin").Length
Write-Host "  TTY kernel = ${kSizeTty}B"

Build-FloppyImg "$bld\saylter-tty.img" "$bld\pm.bin" "$bld\kernel-tty.bin"
Write-Host "  OK saylter-tty.img ($((Get-Item "$bld\saylter-tty.img").Length)B)" -ForegroundColor Green

Write-Host ''
Write-Host '===== IDE disk build ====='
Build-IdeImg "$bld\saylter-ide.img" "$bld\pm.bin" "$bld\kernel-tty.bin"
$f = Get-Item "$bld\saylter-ide.img"
Write-Host "  OK saylter-ide.img (logical $($f.Length)B / sparse, FS LBA$SECTOR_FS)" -ForegroundColor Green