/* ============================================================
 * vc_text.h - VizCormin 编辑器文本核心
 * ------------------------------------------------------------
 * Piece Table：
 *   - 原始缓冲只追加，不原地修改；
 *   - 编辑产生的文本进入追加缓冲；
 *   - 文档由一串 piece（源 + 起点 + 长度）组成。
 * UTF-8 边界、行列映射、撤销/重做、多光标编辑都在这一层完成，
 * 渲染层只消费“可视行 / 光标位置”。
 *
 * 撤销采用完整快照（每步一份），实现简单且不会出现逆向
 * 编辑不精确的问题；后续可替换为增量记录而不改上层 API。
 * ============================================================ */

#ifndef VC_TEXT_H
#define VC_TEXT_H

#include <stdint.h>

typedef struct VcText VcText;

VcText *vc_create(void);
void    vc_destroy(VcText *t);

/* 载入文档（复制到原始缓冲）。返回 1 成功。 */
int     vc_load(VcText *t, const char *data, uint32_t len);

uint32_t vc_length(const VcText *t);

/* 在 pos 处插入/删除；pos 必须 <= 长度。返回 1 成功。 */
int     vc_insert(VcText *t, uint32_t pos, const char *s, uint32_t len);
int     vc_erase(VcText *t, uint32_t pos, uint32_t len);

/* 复制 [pos, pos+len) 到 out，返回实际复制字节数（不超过 cap）。 */
uint32_t vc_copy(const VcText *t, uint32_t pos, uint32_t len,
                 char *out, uint32_t cap);

/* 提取整个文档（调用方提供缓冲区）。 */
uint32_t vc_copy_all(const VcText *t, char *out, uint32_t cap);

/* UTF-8：返回 pos 处字符的字节数（1..4），越界返回 0。 */
uint32_t vc_char_size(const VcText *t, uint32_t pos);

/* 行信息：以 '\n' 分行；末行不需要换行符。 */
uint32_t vc_line_count(const VcText *t);
uint32_t vc_line_start(const VcText *t, uint32_t line);
uint32_t vc_line_of(const VcText *t, uint32_t pos);
uint32_t vc_col_of(const VcText *t, uint32_t pos);   /* UTF-8 字符列 */

/* 撤销/重做：编辑前调用 vc_undo_push，再执行编辑。 */
int     vc_undo_push(VcText *t);
int     vc_undo(VcText *t);
int     vc_redo(VcText *t);
int     vc_can_undo(const VcText *t);
int     vc_can_redo(const VcText *t);

/* 多光标：pos 为插入点，anchor 为选择锚点（相等即无选区）。 */
typedef struct {
    uint32_t pos;
    uint32_t anchor;
} VcCursor;

/* 在每个光标处插入同一段文本，光标随编辑右移。 */
int vc_insert_multi(VcText *t, VcCursor *cursors, int n,
                    const char *s, uint32_t len);

/* 删除每个光标的选区（anchor..pos），并折叠为选区起点。 */
int vc_erase_multi(VcText *t, VcCursor *cursors, int n);

#endif /* VC_TEXT_H */
