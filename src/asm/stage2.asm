; stage2.asm - AurionOS second-stage bootloader (real mode, loaded at 0x7E00)
;
; The 512-byte boot sector only loads things; everything that needs the
; BIOS (VESA detect, RAM detect) plus the protected-mode entry lives here
; where there is room to breathe. Entered with a far jump from the boot
; sector, so CS=0; the stack is still 0x7C00, growing down away from us.

BITS 16
org 0x7E00

%ifndef KERNEL_DEST
%define KERNEL_DEST 0x100000
%endif

; One progress byte to COM1 (see bootload.asm SERPUT).
%macro SERPUT 1
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

stage2_main:
    xor ax, ax
    mov ds, ax
    mov es, ax

    SERPUT '2'                  ; stage 2 running

    ; Clear the VBE result storage area at 0x9000 (32 bytes).
    mov di, 0x9000
    mov cx, 16
    xor ax, ax
    cld
    rep stosw

    ; Detect the best VESA video mode.
    ; Results stored at physical 0x9000 for the kernel to read.

    ; Get VESA BIOS info block into 0x8000
    mov di, 0x8000
    mov dword [di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .enter_pm           ; No VBE - just go with text mode

    ; Walk the mode list (VBE 1.2: seg:off at 0x0A/0x0C; VBE 2.0+: 32-bit ptr at 0x0A)
    mov     ax, [0x8004]
    cmp     ax, 0x0200
    jb      .vbe12_mode_ptr
    ; VBE 2.0+ -> 32-bit linear pointer at 0x0A; lfs works for BIOS-rom addresses
    lfs     si, [0x800A]
    jmp     .mode_ptr_ready
.vbe12_mode_ptr:
    ; VBE 1.2 -> offset at 0x0A, segment at 0x0C
    mov     si, [0x800A]
    mov     fs, [0x800C]
.mode_ptr_ready:
    xor     bx, bx
    mov     [best_rank], bx

.next_mode:
    mov cx, [fs:si]
    add si, 2
    cmp cx, 0xFFFF
    je  .find_done

    ; Get mode info for this mode into 0x8200
    mov di, 0x8200
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .next_mode

    ; Must have: linear framebuffer support (bit 7 of attributes)
    test byte [0x8200], 0x80
    jz  .next_mode

    ; Must be 32-bit color (QEMU is much more stable in 32-bpp path)
    cmp byte [0x8219], 32
    jb  .next_mode

    ; Rank by resolution: prefer 1920x1080, else by pixel count
    mov ax, [0x8212]        ; mode width
    mov dx, [0x8214]        ; mode height
    cmp ax, 1920
    jne .pix
    cmp dx, 1080
    jne .pix
    mov eax, 0x1000000      ; huge rank - always prefer native 1080p
    jmp .compare

.pix:
    mul dx                  ; dx:ax = width * height
    shl edx, 16
    mov dx, ax
    mov eax, edx

.compare:
    cmp eax, [best_rank]
    jbe .next_mode
    mov [best_rank], eax
    mov [best_mode], cx

    ; Store useful mode info for the kernel at 0x9000:
    ;   [0x9000] = framebuffer physical address (dword)
    ;   [0x9004] = width | height packed (dword: lo=width, hi=height)
    ;   [0x9008] = bits per pixel (byte)
    ;   [0x900A] = bytes per scan line / pitch (word)
    mov eax, [0x8228]
    mov [0x9000], eax
    mov eax, [0x8212]
    mov [0x9004], eax
    mov al, [0x8219]
    mov [0x9008], al
    mov ax, [0x8210]
    mov [0x900A], ax
    mov ax, [0x8232]
    mov [0x900C], ax
    jmp .next_mode

.find_done:
    ; Set the best mode (with linear framebuffer flag = bit 14)
    mov     bx, [best_mode]
    or      bx, bx
    jz      .enter_pm
    mov     ax, 0x4F02
    or      bx, 0x4000
    int     0x10
    cmp     ax, 0x004F
    jne     .enter_pm

    SERPUT 'V'                  ; VESA probe + mode set done

.enter_pm:
    ; Detect physical RAM size using BIOS INT 15h, AX=E801h
    mov ax, 0xE801
    int 0x15
    jc  .mem_fallback

    test cx, cx
    jnz .use_cx_dx
    mov cx, ax
    mov dx, bx
.use_cx_dx:
    movzx eax, dx
    shl eax, 6
    add ax, cx
    add eax, 1024
    jmp .mem_done

.mem_fallback:
    mov eax, 65536         ; 64 MB fallback
.mem_done:
    mov [0x9010], eax      ; Store KB total at 0x9010

    SERPUT 'M'                  ; entering protected mode next
    ; Enter 32-bit protected mode and jump to kernel
    cli
    ; Enable A20 line via fast port (most reliable)
    in  al, 0x92
    or  al, 2
    out 0x92, al

    lgdt [gdtp]

    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    jmp 0x08:pm32

halt:
    hlt
    jmp halt

; 32-bit protected mode entry
BITS 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x1F0000       ; Stack below BSS at 2MB, above kernel end
    jmp KERNEL_DEST

; GDT - flat 4GB code and data segments
BITS 16
gdt:
    dq 0                            ; null descriptor
    dq 0x00CF9A000000FFFF           ; 0x08: code, 32-bit, ring 0, 4GB
    dq 0x00CF92000000FFFF           ; 0x10: data, 32-bit, ring 0, 4GB
gdtp:
    dw 23
    dd gdt

; Working variables
best_mode: dw 0
best_rank: dd 0
