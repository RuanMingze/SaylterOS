set pagination off
set confirm off
set logging file build\gdb-caught.log
set logging overwrite on
set logging enabled on

target remote localhost:1234

echo --- 初始状态 ---\n
info registers

echo --- 设断点 ---\n
break *0x7C00
break *0x7E00
break *0x8600

echo --- 捕获 CPU 异常 ---\n
catch exception

echo --- continue 1: 到 0x7C00 ---\n
continue

echo ============================================================\n
echo MBR 入口到达！继续让它跑\n
echo ============================================================\n
info registers
continue

echo ============================================================\n
echo Stage2 或异常！\n
echo ============================================================\n
info registers
x/10i $pc-5
continue

echo ============================================================\n
echo PM 或异常！\n
echo ============================================================\n
info registers
x/10i $pc-5
continue

echo ============================================================\n
echo 崩溃现场！\n
echo ============================================================\n
info registers
x/20i $pc-10
x/32wx $esp
info idt
info gdt

detach
quit
