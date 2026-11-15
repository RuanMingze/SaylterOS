/******************************************************************************
 *
 * Name: acsaylter.h - SaylterOS 平台配置
 *
 * 说明：这是项目自有的平台头（非 ACPICA 官方文件）。升级 ACPICA 后需要
 *       复核 include/platform/acenv.h 中的平台分派分支是否仍然存在，
 *       并确认本文件用到的 ACPICA 配置宏没有被重命名。
 *
 *****************************************************************************/

#ifndef __ACSAYLTER_H__
#define __ACSAYLTER_H__

/*
 * 不使用宿主 C 库：内核是 -nostdlib 环境，ACPICA 自带
 * components/utilities/utclib.c 的本地 str/mem 实现。
 */
#undef ACPI_USE_STANDARD_HEADERS
#undef ACPI_USE_SYSTEM_CLIBRARY

/* x86_64 长模式 */
#define ACPI_MACHINE_WIDTH          64

/*
 * 单地址空间、非抢占式协作调度：
 *   - 互斥用 OSL 实现（AcpiOsAcquireLock/ReleaseLock，空实现）
 *   - 对象缓存用 ACPICA 自带的本地缓存（utcache.c）
 */
#define ACPI_MUTEX_TYPE             ACPI_OSL_MUTEX
#define ACPI_USE_LOCAL_CACHE

/* x86_64 有原生 64 位乘法与除法，无需 utmath.c 的软件回退 */
#define ACPI_USE_NATIVE_MATH64
#define ACPI_USE_NATIVE_DIVIDE

#define COMPILER_DEPENDENT_INT64    long long
#define COMPILER_DEPENDENT_UINT64   unsigned long long

#endif /* __ACSAYLTER_H__ */
