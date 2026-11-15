$img = 'build\saylter-ide.img'
if (-not (Test-Path $img)) {
    Write-Host 'IDE image not found, rebuilding...'
    powershell -NoProfile -File .\build.ps1
}

# 长模式必须使用 x86_64 机器模型；qemu64 支持 PAE + 2MB 页 + 长模式
qemu-system-x86_64 -machine pc -cpu qemu64 -m 64 -display gtk `
  -boot c `
  -drive format=raw,file=$img,if=ide `
  -snapshot -no-reboot
