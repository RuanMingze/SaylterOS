/* ============================================================
 * vc_fs_slyzfs.h - SlyzFS 版 VcFileOps
 * ------------------------------------------------------------
 * 读取时一次性读入内存（vc_alloc），写入时整份覆盖。
 * 路径使用 SlyzFS 的 C:\... / R:\... 形式。
 * ============================================================ */

#ifndef VC_FS_SLYZFS_H
#define VC_FS_SLYZFS_H

#include "../core/vc_session.h"

VcFileOps vc_fs_slyzfs(void);

#endif /* VC_FS_SLYZFS_H */
