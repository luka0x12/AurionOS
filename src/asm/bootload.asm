BITS 16
org 0x7C00

; Boot parameters - override these from the build script with -D flags

%ifndef STAGE2_LBA
%define STAGE2_LBA 1
%endif
%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 1
%endif
%ifndef KERNEL_LBA
%define KERNEL_LBA 2
%endif
%ifndef KERNEL_DEST
%define KERNEL_DEST 0x100000
%endif
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 800
%endif

; Sectors land in a 512-byte bounce buffer first: BIOS CHS reads cannot
; address high memory, so each sector is copied up to KERNEL_DEST with
; 4GB data segments (unreal mode, set up below).

%define BOUNCE_OFF 0x7000
%define STAGE2_ADDR 0x7E00
%ifndef COPY_DWORDS
%define COPY_DWORDS 128
%endif

; One progress byte to COM1. Harmless with no UART attached; with a
; serial log it shows exactly which load stage died. QEMU: -serial file:.
%macro SERPUT 1
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

; FAT12 BPB - required for El Torito floppy emulation.
; The BIOS checks the BPB to determine geometry for INT 13h emulation.
; Values must match the floppy image geometry: 1.44MB, 18 spt, 2 heads.

    jmp short main
    nop

; OEM Name (8 bytes)
    db "AURIONOS"
; BPB_BytsPerSec (2)
    dw 512
; BPB_SecPerClus (1)
    db 1
; BPB_RsvdSecCnt (2)
    dw 1
; BPB_NumFATs (1)
    db 2
; BPB_RootEntCnt (2)
    dw 224
; BPB_TotSec16 (2)
    dw 2880
; BPB_Media (1)
    db 0xF0
; BPB_FATSz16 (2)
    dw 9
; BPB_SecPerTrk (2) = sectors per track
spt: dw 18
; BPB_NumHeads (2) = number of heads
nhd: dw 2
; BPB_HiddSec (4)
    dd 0
; BPB_TotSec32 (4)
    dd 0
; BS_DrvNum (1) - BIOS may overwrite this with DL=0x00
drv: db 0
; BS_Reserved1 (1)
    db 0
; BS_BootSig (1)
    db 0x29
; BS_VolID (4)
    dd 0x12345678
; BS_VolLab (11)
    db "AURION OS  "
; BS_FilSysType (8)
    db "FAT12   "

; Bootloader entry point
main:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; Stack just below the bootloader - grows downward safely into free RAM
    mov sp, 0x7C00
    sti

    ; Save the drive number the BIOS gave us.
    ; In El Torito floppy emulation mode this is always 0x00 (virtual floppy A:).
    ; In El Torito no-emulation mode this would be the CD-ROM (0xE0 or similar).
    ; We only support floppy emulation now so we force DL=0x00 here to be safe.
    mov byte [drv], 0x00

    ; The kernel no longer fits below 0xA0000 (VGA RAM starts there), so it
    ; loads at 1MB. Real-mode CHS reads cannot address that high: each
    ; sector goes to a bounce buffer at 0x7000 first, then is copied up.
    ; A20 must be on before the first copy or 0x100000 wraps to 0x00000.
    in  al, 0x92
    or  al, 2
    out 0x92, al

    ; GDTR for the per-sector unreal flips in the kernel loop below.
    ; DS stays plain 0 the whole time (all variables live below 64K);
    ; only ES is flipped to 4GB, and only around each high copy, because
    ; a BIOS call preserves segment values but not descriptor caches.
    lgdt [gdtp]

    ; Stage 2 (BIOS services + protected-mode entry) loads to 0x7E00,
    ; right after this sector. ES carries base 0 either way down here,
    ; so plain BX offsets address it directly.
    mov dword [lba], STAGE2_LBA
    mov di, STAGE2_ADDR
    mov cx, STAGE2_SECTORS

.s2_loop:
    push cx
    push di

    ; Flat LBA -> CHS: cyl = lba/(spt*nhd), head = (lba/spt)%nhd,
    ; sector = (lba%spt)+1.
    mov ax, [lba]
    xor dx, dx
    div word [spt]
    inc dx
    mov cl, dl
    xor dx, dx
    div word [nhd]
    mov ch, al
    mov dh, dl

    mov dl, byte [drv]
    mov bx, di

    mov si, 3
.s2_retry:
    mov ax, 0x0201
    int 0x13
    jnc .s2_ok
    dec si
    jz halt
    xor ax, ax
    int 0x13
    jmp .s2_retry

.s2_ok:
    pop di
    add di, 512
    inc dword [lba]
    pop cx
    loop .s2_loop

    SERPUT 'S'                  ; stage 2 image is in place at 0x7E00

    ; Kernel sectors follow stage 2 on disk and land at 1MB in RAM.
    ; The BIOS read runs with plain ES=0; the 4GB ES for the high copy
    ; is established after the read returns, with interrupts off, since
    ; neither the BIOS nor an IRQ preserves descriptor caches.
    mov dword [lba], KERNEL_LBA
    mov dword [hi_dest], KERNEL_DEST
    mov cx, KERNEL_SECTORS

.read_loop:
    push cx

    ; Convert flat LBA to CHS for INT 13h (same math as above).
    mov ax, [lba]
    xor dx, dx
    div word [spt]          ; ax = lba/spt, dx = lba%spt
    inc dx                  ; sector is 1-based
    mov cl, dl              ; CL = sector number
    xor dx, dx
    div word [nhd]          ; ax = cylinder, dx = head
    mov ch, al              ; CH = cylinder (low 8 bits)
    mov dh, dl              ; DH = head

    ; DL = drive (0x00 = floppy), ES:BX = bounce buffer at linear 0x7000
    xor ax, ax
    mov es, ax
    mov dl, byte [drv]
    mov bx, BOUNCE_OFF

    ; INT 13h Read - retry up to 3 times on failure
    mov si, 3
.retry:
    mov ax, 0x0201          ; AH=02 (read), AL=01 (1 sector)
    int 0x13
    jnc .ok                 ; carry clear = success
    dec si
    jz  halt                ; tried 3 times, give up
    ; Reset the floppy drive before retrying
    xor ax, ax
    int 0x13
    jmp .retry

.ok:
    ; Bounce buffer -> high memory, 128 dwords, then back to plain ES
    ; for the next BIOS read. CLD first: the BIOS may leave DF set.
    cli
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    mov bx, 0x10
    mov es, bx
    dec eax
    mov cr0, eax
    cld
    mov esi, BOUNCE_OFF
    mov edi, [hi_dest]
    mov ecx, COPY_DWORDS
    ; a32 is the whole fix: in BITS 16 `rep movsd' defaults to SI/DI,
    ; so without it DI truncates EDI=0x100000 to 0x0000 and the copy
    ; overwrites the IVT instead of the kernel destination.
    a32 rep movsd
%ifdef COPY_PROBE
    ; Read back the last dword through both segments: equal means ES
    ; really has base 0 and the copy landed at [hi_dest].
    mov eax, [es:edi-4]
    mov ebx, [esi-4]
    cmp eax, ebx
    je .probe_ok
    SERPUT 'E'
.probe_hang:
    hlt
    jmp .probe_hang
.probe_ok:
%endif
    sti
    SERPUT 'a'
    xor bx, bx
    mov es, bx
    add dword [hi_dest], 512
    inc dword [lba]
    pop cx
    dec cx
    jnz near .read_loop

    SERPUT 'K'                  ; kernel is up at 1MB, handing to stage 2

    ; Disk work is done. Stage 2 takes over: VESA detect, RAM detect,
    ; protected mode, jump to the kernel. It sets its own segments.
    jmp 0x0000:STAGE2_ADDR

halt:
    SERPUT 'h'                  ; a disk read failed 3 times; see serial log
    hlt
    jmp halt

; GDT - flat 4GB code and data segments (only used for unreal mode
; above; stage 2 carries its own copy for the protected-mode entry)

gdt:
    dq 0                            ; null descriptor
    dq 0x00CF9A000000FFFF           ; 0x08: code, 32-bit, ring 0, 4GB
    dq 0x00CF92000000FFFF           ; 0x10: data, 32-bit, ring 0, 4GB
gdtp:
    dw 23
    dd gdt

; Working variables
lba:       dd 0
hi_dest:   dd 0

times 510-($-$$) db 0
dw 0xAA55