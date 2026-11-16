#ifndef VM_H
#define VM_H

#include <stdint.h>

/* ============================================================
 * vm.h - 4KB 分页 / 虚拟内存 / 按需分页 / swap
 * ------------------------------------------------------------
 * 设计要点：
 *   1. 引导阶段（pm.asm）建立的 4GB 恒等映射全部使用 2MB 大页，
 *      大页无法表达"单页不存在"，因此按需分页区在启用时必须先把
 *      对应 PD 项拆成 4KB 页表。
 *   2. 按需分页区是一个独立的虚拟地址窗口（默认 0x20000000），
 *      物理页来自 24MB 以上的空闲 RAM 帧池，二者完全解耦。
 *   3. 缺页处理走 #PF 专用入口：pm.asm 保存现场后调用
 *      page_fault_handler()，返回 1 表示已修复，iretq 继续执行。
 *   4. 物理驻留页数受配额约束（默认 1536 页 = 6MB）。达到配额时
 *      按二次机会（clock）算法换出到 swap 文件，而不是直接 OOM。
 * ============================================================ */

#define VM_PAGE_SIZE          4096u
#define VM_DEMAND_BASE        0x20000000u
#define VM_DEMAND_PAGES       4096u        /* 16MB 按需分页窗口 */
#define VM_RESIDENT_LIMIT     2048u        /* 默认驻留配额（8MB） */
#define VM_SWAP_SLOTS         4096u        /* pagefile 容量 4096 个 4KB 槽（16MB） */

/* 初始化：探测 RAM、建立帧池、拆分按需区页表、挂载 pagefile。
 * 必须在 SlyzFS 挂载之后、登录之前调用（swap 文件创建需要 root 身份）。 */
void vm_init(void);

/* #PF C 侧处理函数。fault_addr 取自 CR2，error_code 为 CPU 压栈的错误码，
 * rip 为触发缺页的指令地址。返回 1 = 已修复可续跑，0 = 致命。 */
int page_fault_handler(uint64_t fault_addr, uint64_t error_code, uint64_t rip);

/* 供 shell 使用的查询与操作接口 */
int vm_ready(void);
void vm_dump(void);
void vm_dump_swap(void);
int vm_selftest(void);
int vm_set_resident_limit(uint32_t pages);
uint32_t vm_demand_pages(void);
uint32_t vm_resident_pages(void);
uint32_t vm_fault_count(void);
uint32_t vm_swapout_count(void);
uint32_t vm_swapin_count(void);

/* 设备信息 / 任务管理器使用的只读查询 */
uint64_t vm_total_ram(void);
void     vm_frame_stats(uint32_t *total, uint32_t *free_frames, uint32_t *resident);
uint32_t vm_swap_slots_used(void);
uint32_t vm_swap_slot_capacity(void);

#endif /* VM_H */
