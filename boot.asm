; boot.asm - Tidepool OS: điểm vào Multiboot2 -> chuyển CPU sang long mode 64-bit -> gọi kernel_main.
;
; GRUB nạp nhân ở chế độ protected mode 32-bit, nên phần đầu file này là code 32-bit.
; Các bước: kiểm tra CPU -> dựng bảng trang (identity map 4 GiB) -> bật PAE + long mode -> nhảy vào 64-bit.
; Dựng 4 GiB (chứ không chỉ vài MiB) vì framebuffer của máy ảo thường nằm ở vùng 0xFC000000+.

MB2_MAGIC   equ 0xE85250D6          ; số thần kỳ của header Multiboot2
MB2_ARCH    equ 0                   ; 0 = i386 protected mode
MB2_BOOTED  equ 0x36D76289          ; GRUB đặt giá trị này vào EAX khi đã nạp nhân đúng chuẩn

; ---------------------------------------------------------------------------
; Header Multiboot2 (phải nằm trong 32 KiB đầu của file; linker.ld đặt nó lên đầu)
; ---------------------------------------------------------------------------
section .multiboot_header
align 8
mb2_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd mb2_end - mb2_start
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH + (mb2_end - mb2_start))

    ; Yêu cầu GRUB dựng sẵn framebuffer 1024x768, 32 bit/pixel (flags=1: không bắt buộc)
    dw 5                            ; type = framebuffer
    dw 1                            ; flags = optional
    dd 20                           ; size
    dd 1024                         ; width
    dd 768                          ; height
    dd 32                           ; depth
    dd 0                            ; đệm cho tag kế tiếp thẳng hàng 8 byte

    ; Tag kết thúc
    dw 0
    dw 0
    dd 8
mb2_end:

; ---------------------------------------------------------------------------
; Bộ nhớ chưa khởi tạo: bảng trang, ngăn xếp, con trỏ thông tin boot
; ---------------------------------------------------------------------------
section .bss align=4096
alignb 4096
pml4:       resb 4096
pdpt:       resb 4096
pd:         resb 4096 * 4           ; 4 bảng PD x 512 mục x 2 MiB = 4 GiB
alignb 16
stack_bottom: resb 16384
stack_top:
mb_info:    resq 1

; ---------------------------------------------------------------------------
; Code 32-bit
; ---------------------------------------------------------------------------
section .text
bits 32
global _start
extern kernel_main

_start:
    cli
    mov esp, stack_top
    mov [mb_info], ebx              ; lưu địa chỉ cấu trúc thông tin Multiboot2 (EBX)
    cmp eax, MB2_BOOTED
    jne .no_multiboot

    ; CPU có lệnh CPUID không? (thử lật bit 21 của EFLAGS)
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid

    ; CPU có long mode (x86_64) không?
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; PML4[0] -> PDPT
    mov eax, pdpt
    or eax, 0b11                    ; present | writable
    mov [pml4], eax

    ; PDPT[0..3] -> pd + i*4096
    xor ecx, ecx
.map_pdpt:
    mov eax, 4096
    mul ecx
    add eax, pd
    or eax, 0b11
    mov [pdpt + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .map_pdpt

    ; 2048 trang 2 MiB liên tiếp: identity map 0 .. 4 GiB
    xor ecx, ecx
.map_pd:
    mov eax, 0x200000
    mul ecx                         ; EDX:EAX = ecx * 2 MiB
    or eax, 0b10000011              ; present | writable | huge (2 MiB)
    mov [pd + ecx * 8], eax
    mov [pd + ecx * 8 + 4], edx
    inc ecx
    cmp ecx, 2048
    jne .map_pd

    ; Bật phân trang 64-bit: CR3 -> PML4, CR4.PAE, EFER.LME, CR0.PG
    mov eax, pml4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5                  ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080             ; MSR EFER
    rdmsr
    or eax, 1 << 8                  ; LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31                 ; PG
    mov cr0, eax

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start  ; far jump nạp CS 64-bit

; Lỗi sớm: in "ERR: <mã>" ra VGA text rồi dừng. M = không phải Multiboot2, C = không có CPUID, L = không có long mode.
.no_multiboot:
    mov al, 'M'
    jmp .error
.no_cpuid:
    mov al, 'C'
    jmp .error
.no_long_mode:
    mov al, 'L'
.error:
    mov dword [0xB8000], 0x4F524F45
    mov dword [0xB8004], 0x4F3A4F52
    mov dword [0xB8008], 0x4F204F20
    mov byte  [0xB800A], al
.hang:
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; Code 64-bit
; ---------------------------------------------------------------------------
bits 64
long_mode_start:
    xor eax, eax                    ; long mode không dùng DS/ES/SS: nạp selector rỗng
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov edi, [mb_info]              ; đối số 1 (System V ABI): địa chỉ thông tin Multiboot2
    call kernel_main                ; không bao giờ trở về
.halt:
    cli
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
; GDT tối thiểu cho long mode: mục 0 rỗng + một đoạn code 64-bit
; ---------------------------------------------------------------------------
section .rodata
align 8
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)   ; executable | code/data | present | long mode
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; Báo cho linker biết nhân không cần ngăn xếp thực thi được
section .note.GNU-stack noalloc noexec nowrite progbits
