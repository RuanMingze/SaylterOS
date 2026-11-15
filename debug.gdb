set pagination off
set logging file build\gdb-crash2.log
set logging overwrite on
set logging enabled on

echo ============================================================\n
echo [1] 连接 QEMU + continue 让它跑起来\n
echo ============================================================\n
target remote localhost:1234

echo --- 初始状态 ---\n
info registers
echo --- 当前指令 ---\n
x/5i $pc

echo ============================================================\n
echo [2] 继续运行，等待崩溃/断点\n
echo ============================================================\n
continue

echo ============================================================\n
echo [3] 崩溃现场！\n
echo ============================================================\n
echo --- 寄存器 ---\n
info registers
echo --- 当前指令 ---\n
x/20i $pc-10
echo --- 栈 ---\n
x/32wx $esp
echo --- 0x7C00 MBR ---\n
x/32bx 0x7C00
echo --- 0x7E00 Stage2 ---\n
x/32bx 0x7E00
echo --- 0x8600 PM ---\n
x/32bx 0x8600
echo --- 0xA200 Kernel ---\n
x/32bx 0xA200

detach
quit
