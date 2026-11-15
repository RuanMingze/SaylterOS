$img = 'build\saylter-ide.img'
if (-not (Test-Path $img)) {
    Write-Host 'IDE image not found, rebuilding...'
    powershell -NoProfile -File .\build.ps1
}

qemu-system-i386 -machine pc -cpu pentium2 -m 64 -display gtk `
  -boot c `
  -drive format=raw,file=$img,if=ide `
  -snapshot -no-reboot
