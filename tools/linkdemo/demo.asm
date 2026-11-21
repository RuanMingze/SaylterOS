; ============================================================
; demo.asm - 动态链接测试应用
; ------------------------------------------------------------
; 加载地址 0x06020000，导入 slyz_add。
; 入口收到 SlysKernelApi*（rcx），调用 2 + 3 并通过
; api->log_u32 把结果打到串口。
;
; SlysKernelApi 偏移：
;   +40 log(const char *)
;   +48 log_u32(uint32_t)
; ============================================================
BITS 64
default abs

org 0x06020000

entry:
    push rbx
    sub rsp, 32                  ; Win64 shadow space

    mov rbx, rcx                 ; api

    mov ecx, 2
    mov edx, 3
    mov rax, [0x06020080]        ; 导入槽：加载器回填 slyz_add
    call rax

    mov rcx, rax
    mov rax, [rbx + 48]          ; api->log_u32
    call rax

    mov rcx, msg
    mov rax, [rbx + 40]          ; api->log
    call rax

    xor eax, eax
    add rsp, 32
    pop rbx
    ret

; 导入槽固定在镜像偏移 0x80
times 0x80 - ($ - $$) db 0
import_slyz_add:
    dq 0

msg:
    db 13, 10, 0
