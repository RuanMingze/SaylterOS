set pagination off
set confirm off
set logging file build\gdb-step.log
set logging overwrite on
set logging enabled on

target remote localhost:1234

echo --- 清所有断点和 catch ---\n
info breakpoints

echo --- 只在 0x8600 设断点，到了就单步 ---\n
break *0x8600
continue

echo ============================================================\n
echo 到达 pm.asm 入口！开始单步\n
echo ============================================================\n
info registers

echo --- 单步 1: cli ---\n
stepi
info registers
echo --- 单步 2: xor ax,ax ---\n
stepi
echo --- 单步 3: mov ds,ax ---\n
stepi
echo --- 单步 4: mov es,ax ---\n
stepi
echo --- 单步 5: mov ss,ax ---\n
stepi
echo --- 单步 6: mov sp,0x8000 ---\n
stepi
info registers

echo --- 单步 7: lgdt [gdt_descriptor] ---\n
stepi
echo --- GDT 加载后检查 ---\n
x/8bx 0x8815
info registers

echo --- 单步 8: mov eax,cr0 ---\n
stepi
echo --- 单步 9: or eax,1 ---\n
stepi
echo --- 单步 10: mov cr0,eax (开保护模式!) ---\n
stepi
echo --- cr0 应该变了 ---\n
info registers

echo --- 单步 11: far jump db 0xEA ... 跳到 pm_entry ---\n
echo ============================================================\n
echo 如果这里断了,就是 far jump 触发的 GPF!\n
echo ============================================================\n
stepi

echo ============================================================\n
echo 成功进入保护模式!\n
echo ============================================================\n
info registers
x/10i $pc-5

detach
quit
