BITS 32

global smp_gdt
global smp_install_gdt_bsp
global smp_ap_lidt
global smp_rdmsr
global smp_wrmsr
global smp_ap_entry_wrap

extern smp_ap_entry
extern idt_desc

section .data
align 16
smp_gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
smp_gdt_end:

smp_gdtr_int:
    dw smp_gdt_end - smp_gdt - 1
    dd smp_gdt

section .tramp
align 16
global smp_tramp_image
global smp_tramp_end
smp_tramp_image:
incbin "build/smp_tramp.bin"
smp_tramp_end:

section .text

; BSP adopts the kernel-owned GDT so APs share the same descriptors.
; Descriptors are identical to the bootloader GDT, interrupts off by caller.
smp_install_gdt_bsp:
    push ebp
    mov ebp, esp
    pushfd
    cli
    lgdt [smp_gdtr_int]
    jmp 0x08:.reload
.reload:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    popfd
    leave
    ret

; APs share the BSP IDT. Called once per AP with interrupts disabled.
smp_ap_lidt:
    lidt [idt_desc]
    ret

; void smp_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
smp_rdmsr:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    mov ecx, [ebp + 8]
    rdmsr
    mov esi, [ebp + 12]
    mov [esi], eax
    mov esi, [ebp + 16]
    mov [esi], edx
    pop esi
    pop ebx
    leave
    ret

; void smp_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
smp_wrmsr:
    push ebp
    mov ebp, esp
    push ebx
    mov ecx, [ebp + 8]
    mov eax, [ebp + 12]
    mov edx, [ebp + 16]
    wrmsr
    pop ebx
    leave
    ret

; Never called directly; documents the AP calling convention used by the
; trampoline mailbox (esp set, jmp to smp_ap_entry, never returns).
smp_ap_entry_wrap:
    call smp_ap_entry
.hang:
    hlt
    jmp .hang
