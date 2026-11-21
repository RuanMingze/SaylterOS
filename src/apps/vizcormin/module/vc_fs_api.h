/* ============================================================
 * vc_fs_api.h - 独立模块的 SlyzFS 文件接口（经 KernelApi）
 * ============================================================ */

#ifndef VC_FS_API_H
#define VC_FS_API_H

#include "slys.h"
#include "../core/vc_session.h"

void      vc_fs_api_bind(const struct SlysKernelApi *api);
VcFileOps vc_fs_api(void);

#endif /* VC_FS_API_H */
