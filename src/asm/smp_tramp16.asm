; smp_tramp16.asm - AP bootstrap trampoline, assembled flat at 0x8000.
; BSP copies the 512-byte image to 0x8000, fills the mailbox at 0x7000
; (GDTR 6B @0x7000, protected entry u32 @0x7006, stack top u32 @0x700A),
; then sends INIT-SIPI-SIPI with vector 0x08. APs are booted one at a
; time so the single stack slot is race-free. Never returns.

BITS 16
org 0x8000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    lgdt [0x7000]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:prot

BITS 32
prot:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Firmware starts APs with CR0.CD+NW set (uncached). An uncached
    ; rasterizer looks frozen, so I clear CD/NW, set MP and init x87.
    ; Same sequence runs on the BSP in smp_init for real-hardware safety.
    mov eax, cr0
    and eax, 0x9FFFFFFF
    or eax, 0x2
    mov cr0, eax
    wbinvd
    fninit
    mov esp, [0x700A]
    jmp dword [0x7006]

times 512-($-$$) db 0
