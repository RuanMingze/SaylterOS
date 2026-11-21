/* ============================================================
 * vc_session.h - VizCormin 文档与标签页会话
 * ------------------------------------------------------------
 * 每个文档 = 文本模型 + 视图模型 + 路径 + 脏标记 + 光标/选区。
 * 文件读写通过 VcFileOps 抽象：
 *   - 宿主测试用内存文件系统；
 *   - SaylterOS 用 SlyzFS 实现。
 * 编辑必须走 vc_doc_* 包装，统一处理撤销、脏标记与视图重建。
 * ============================================================ */

#ifndef VC_SESSION_H
#define VC_SESSION_H

#include <stdint.h>
#include "vc_text.h"
#include "vc_view.h"
#include "vc_find.h"

#define VC_PATH_MAX 260

typedef struct {
    void *user;
    /* 成功返回 1，*out 用 vc_alloc 分配，调用方 vc_free。 */
    int (*read)(void *user, const char *path, char **out, uint32_t *len);
    int (*write)(void *user, const char *path, const char *data, uint32_t len);
    int (*exists)(void *user, const char *path);
} VcFileOps;

typedef struct {
    VcText   *text;
    VcView   *view;
    char      path[VC_PATH_MAX];
    int       dirty;
    uint32_t  cursor;
    uint32_t  anchor;
    uint32_t  x_scroll;
} VcDocument;

typedef struct VcSession VcSession;

VcSession *vc_session_create(const VcFileOps *ops, uint32_t cols, uint32_t rows);
void       vc_session_destroy(VcSession *s);

void       vc_session_set_size(VcSession *s, uint32_t cols, uint32_t rows);

int        vc_session_count(const VcSession *s);
int        vc_session_active_index(const VcSession *s);
VcDocument *vc_session_active(VcSession *s);
VcDocument *vc_session_at(VcSession *s, int index);

/* 打开文件并激活；已打开则直接激活。返回标签页下标，失败 -1。 */
int        vc_session_open(VcSession *s, const char *path);
/* 新建空白文档并激活，返回下标。 */
int        vc_session_new(VcSession *s);
/* 保存；path 为 0 时用文档现有路径。返回 1 成功。 */
int        vc_session_save(VcSession *s, int index, const char *path);
/* 关闭标签页；保证至少保留一个文档。 */
void       vc_session_close(VcSession *s, int index);
void       vc_session_activate(VcSession *s, int index);

/* 编辑包装：自动撤销快照、脏标记、视图重建。 */
int        vc_doc_insert(VcSession *s, VcDocument *d, const char *bytes, uint32_t len);
int        vc_doc_erase_selection(VcSession *s, VcDocument *d);
int        vc_doc_undo(VcDocument *d);
int        vc_doc_redo(VcDocument *d);

/* 替换：整次操作只产生一个撤销步骤。返回替换数量。 */
int        vc_doc_replace_all(VcDocument *d, const char *q, uint32_t qlen,
                              VcFindOptions opt, const char *rep, uint32_t rep_len);
int        vc_doc_replace_one(VcDocument *d, const char *q, uint32_t qlen,
                              VcFindOptions opt, const char *rep, uint32_t rep_len,
                              uint32_t from);

/* 光标移动：按字符/行/页/文档，extend=1 时保留锚点。 */
void       vc_doc_move(VcDocument *d, int unit, int dir, int extend);
void       vc_doc_select_all(VcDocument *d);

#endif /* VC_SESSION_H */
