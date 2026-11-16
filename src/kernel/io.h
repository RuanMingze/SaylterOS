#ifndef IO_H
#define IO_H

#include <stdint.h>

/* x86 I/O port byte-level access —— 所有内联，直接落汇编 */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* 设备 MMIO 屏障：保证描述符/缓冲区写入在触发门铃（写 MMIO 尾指针）
 * 之前已经真正落到内存。x86 的强序只能保证写-写顺序，但编译器仍可能
 * 把普通内存写重排到 volatile MMIO 写之后，必须显式加编译器屏障。 */
static inline void io_memory_barrier(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

#endif /* IO_H */
