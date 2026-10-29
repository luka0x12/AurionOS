# AurionOS Makefile
# Author: AurionOS Development Team
# Purpose: Build AurionOS from assembly, C, and Rust sources into bootable floppy image
# Target: Bootable 1.44MB floppy disk image

# Tool Configuration
NASM        := nasm
GCC         := gcc
LD          := gcc
DD          := dd
OBJCOPY     := objcopy
HEXDUMP     := hexdump
GENISOIMAGE := genisoimage
MKISOFS     := mkisofs
CARGO       := cargo

# OS Detection
# Some WSL setups export OS=Windows_NT, so detect WSL first and prioritize it.
# We still use Windows-native QEMU for CD-ROM on WSL to avoid known ATAPI issues.
IS_WSL := $(shell uname -r 2>/dev/null | grep -qi microsoft && echo yes)

ifeq ($(IS_WSL),yes)
    # WSL2 - build tools are Linux/ELF, but CD-ROM QEMU can be Windows binary.
    NASMFLAGS_ELF := -f elf32 -g -F dwarf
    LDFLAGS := -m32 -nostdlib -T link.ld -Wl,--oformat=binary -Wl,--gc-sections
    PYTHON := python3
    MKDIR := mkdir -p
    RM := rm -f
    RMDIR := rm -rf
    QEMU := qemu-system-i386
    QEMU_ISO := /mnt/c/Program Files/qemu/qemu-system-i386.exe
else ifeq ($(OS),Windows_NT)
    NASMFLAGS_ELF := -f elf32 -g
    LDFLAGS := -m32 -nostdlib -T link.ld
    PYTHON := python
    MKDIR := mkdir
    RM := del /Q
    RMDIR := rmdir /S /Q
    QEMU := qemu-system-i386
    QEMU_ISO := qemu-system-i386
else
    # Native Linux
    NASMFLAGS_ELF := -f elf32 -g -F dwarf
    LDFLAGS := -m32 -nostdlib -T link.ld -Wl,--oformat,binary -Wl,--gc-sections
    PYTHON := python3
    MKDIR := mkdir -p
    RM := rm -f
    RMDIR := rm -rf
    QEMU := qemu-system-i386
    QEMU_ISO := qemu-system-i386
endif

NASMFLAGS_BIN := -f bin
PLATFORM_TAG := $(if $(IS_WSL),wsl,$(if $(filter Windows_NT,$(OS)),windows,linux))

# Normalize paths for shell commands that run under Windows cmd.exe.
pathfix = $(if $(filter Windows_NT,$(OS)),$(subst /,\,$1),$1)

# GCC flags for 32-bit freestanding
# NOTE: Kernel loads at 0x100000 via bounce buffer; .text+.data must stay
# below BSS at 0x200000, so the link recipe fails the build past 960KiB.
CFLAGS := -m32 -ffreestanding -nostdlib -Iinclude -fno-builtin -fno-stack-protector
CFLAGS += -Isrc/gui -Isrc/core -Isrc/fs -Isrc/crypto -Isrc/apps -Isrc/drivers -Isrc/coding -Isrc/kernel_panic -I.
CFLAGS += -Os -Wall -Wextra -std=c11
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
CFLAGS += -fno-pie -fno-pic
CFLAGS += -mpreferred-stack-boundary=2 -mno-mmx -mno-sse -mno-sse2
CFLAGS += -c

# Library path for compiler helpers (floating point, libgcc etc)
LIBGCC  := $(shell $(GCC) -m32 -print-libgcc-file-name)
LIBM    := $(shell $(GCC) -m32 -print-file-name=libm.a)
# Fallback to 32-bit path if auto-detection fails
ifeq ($(LIBGCC),)
    LIBGCC := /usr/lib/gcc/x86_64-linux-gnu/13/32/libgcc.a
endif
# -print-libgcc-file-name silently falls back to the host x86_64 archive when
# the 32-bit multilib variant is not installed (gcc-multilib missing). Linking
# that into a -m32 kernel resolves nothing and would fail with an architecture
# mismatch if a member were ever pulled in, so drop it when positively
# identified; src/core/libgcc_stubs.c supplies the integer helpers instead.
LIBGCC_IS_X86_64 := $(shell m=`ar t "$(LIBGCC)" 2>/dev/null | head -n1`; \
    if [ -n "$$m" ]; then ar p "$(LIBGCC)" "$$m" 2>/dev/null | file - 2>/dev/null | grep -q 'ELF 64-bit' && echo yes; fi)
ifeq ($(LIBGCC_IS_X86_64),yes)
    LIBGCC :=
endif
ifeq ($(LIBM),)
    LIBM := /usr/lib/i386-linux-gnu/libm.a
endif

# Linker flags
# LDFLAGS set above based on OS

# Directories and Files
SRC_DIR     := src
DRIVER_DIR  := Drivers
BUILD_DIR   := build
OBJ_DIR     := $(BUILD_DIR)/obj

# Output files
BOOT_BIN    := $(BUILD_DIR)/bootload.bin
STAGE2_BIN  := $(BUILD_DIR)/stage2.bin
KERNEL_BIN  := $(BUILD_DIR)/kernel.bin
FLOPPY_IMG  := $(BUILD_DIR)/aurionos.img
ISO_IMG     := $(BUILD_DIR)/aurionos.iso
ISO_DIR     := $(BUILD_DIR)/iso

HDD_IMG     := $(BUILD_DIR)/aurionos_hdd.img
HDD_SIZE_MB ?= 60
# PNG wallpapers (kernel reads ISO /Wallpaper/*.png or raw HDD slots)
WALLPAPER_0 := Wallpaper/Night_ridge.png
WALLPAPER_1 := Wallpaper/Misty_conife.png
WALLPAPER_2 := Wallpaper/Gate_B14_217_AM.png
WALLPAPER_3 := Wallpaper/Sunday_4_47_PM.png
WALLPAPER_INS := Wallpaper/ins.png
WALLPAPER_PNGS := $(WALLPAPER_0) $(WALLPAPER_1) $(WALLPAPER_2) $(WALLPAPER_3) $(WALLPAPER_INS)

# Image parameters
IMG_SIZE_SECTORS := 2880           # 1.44MB floppy (2880 * 512 bytes)
BOOT_SIZE        := 512            # Boot sector size
KERNEL_START     := 1              # Kernel starts at sector 1

# Source Files
# Assembly sources (src/asm/)
ASM_BOOT   := $(SRC_DIR)/asm/bootload.asm
ASM_KERNEL := $(SRC_DIR)/asm/kernel.asm \
              $(SRC_DIR)/asm/memory.asm \
              $(SRC_DIR)/asm/filesys.asm \
              $(SRC_DIR)/asm/io.asm \
              $(SRC_DIR)/asm/interrupt.asm \
               $(SRC_DIR)/asm/smp_low.asm \
               $(SRC_DIR)/asm/vesa.asm

# AP bootstrap trampoline: raw 512-byte image at 0x8000, embedded via
# .tramp section in smp_low.asm. Built first, before any kernel objects.
TRAMP_SRC   := $(SRC_DIR)/asm/smp_tramp16.asm
TRAMP_BIN   := $(BUILD_DIR)/smp_tramp.bin

# Stage 2: real-mode BIOS services (VESA/RAM detect) + PM entry at 0x7E00.
# Lives in floppy sectors right after the 512-byte boot sector so the boot
# sector stays small enough to also carry the high-memory kernel loader.
STAGE2_SRC  := $(SRC_DIR)/asm/stage2.asm

# C sources — organized by domain
# drivers/ — hardware drivers (vbe_graphics.c MUST come first to override weak symbols)
# gui/ — desktop, window manager, login, terminal, boot screen
# core/ — commands, shell, syscall, utils, installer, panic
# fs/ — filesystem (FAT32, ISO9660)
# crypto/ — security primitives (PBKDF2, CSPRNG)
# apps/ — applications (3D demo, Blaze browser, etc.)
C_SOURCES  := $(SRC_DIR)/drivers/vbe_graphics.c \
              $(SRC_DIR)/drivers/vbe_extras.c \
              $(SRC_DIR)/drivers/mouse.c \
               $(SRC_DIR)/drivers/ata.c \
               $(SRC_DIR)/drivers/png.c \
              $(SRC_DIR)/drivers/icons.c \
              $(SRC_DIR)/drivers/vmware_svga.c \
              $(SRC_DIR)/drivers/sb16.c \
              $(SRC_DIR)/drivers/sb16_stream.c \
              $(SRC_DIR)/drivers/pcm_engine.c \
              $(SRC_DIR)/drivers/pcm_locate.c \
              $(SRC_DIR)/drivers/pcm_start.c \
              $(SRC_DIR)/drivers/pcm_feed.c \
              $(SRC_DIR)/drivers/pcm_skip.c \
              $(SRC_DIR)/drivers/mp3/audio_glue.c \
              $(SRC_DIR)/drivers/mp3/audio_init.c \
              $(SRC_DIR)/drivers/mp3/audio_play.c \
              $(SRC_DIR)/drivers/mp3/audio_poll.c \
              $(SRC_DIR)/drivers/mp3/audio_irq.c \
              $(SRC_DIR)/drivers/mp3/audio_beep.c \
              $(SRC_DIR)/gui/icon_loader.c \
              $(SRC_DIR)/gui/boot_screen.c \
              $(SRC_DIR)/gui/login_screen.c \
              $(SRC_DIR)/gui/window_manager.c \
              $(SRC_DIR)/gui/menu_bar.c \
              $(SRC_DIR)/gui/desktop.c \
              $(SRC_DIR)/gui/terminal.c \
              $(SRC_DIR)/gui/gui_apps.c \
              $(SRC_DIR)/gui/scrollback.c \
              $(SRC_DIR)/core/commands.c \
              $(SRC_DIR)/core/shell.c \
              $(SRC_DIR)/core/syscall.c \
              $(SRC_DIR)/core/console_shim.c \
              $(SRC_DIR)/core/utils.c \
              $(SRC_DIR)/core/handlers.c \
               $(SRC_DIR)/core/pci.c \
                $(SRC_DIR)/core/scheduler.c \
               $(SRC_DIR)/core/smp.c \
              $(SRC_DIR)/core/clipboard.c \
              $(SRC_DIR)/core/kb_poll.c \
              $(SRC_DIR)/core/installer.c \
              $(SRC_DIR)/core/reinstall.c \
              $(SRC_DIR)/core/firmware_loader.c \
              $(SRC_DIR)/core/libm_stubs.c \
              $(SRC_DIR)/core/libgcc_stubs.c \
              $(SRC_DIR)/drivers/vga_fallback.c \
               $(SRC_DIR)/fs/fat32.c \
               $(SRC_DIR)/fs/iso9660.c \
               $(SRC_DIR)/fs/fs_usage.c \
               $(SRC_DIR)/fs/fs_maintenance.c \
               $(SRC_DIR)/crypto/security.c \
               $(SRC_DIR)/apps/app_3d_demo.c \
               $(SRC_DIR)/apps/BlockForge.c \
               $(SRC_DIR)/apps/app_task_manager.c \
               $(SRC_DIR)/apps/cmd_make.c \
               $(SRC_DIR)/apps/Blaze/blaze_core.c \
              $(SRC_DIR)/apps/Blaze/blaze_html.c \
              $(SRC_DIR)/apps/Blaze/blaze_css.c \
              $(SRC_DIR)/apps/Blaze/blaze_layout.c \
              $(SRC_DIR)/apps/Blaze/blaze_render.c \
              $(SRC_DIR)/apps/Blaze/blaze_net.c \
               $(SRC_DIR)/apps/Blaze/blaze_js.c \
               $(SRC_DIR)/apps/Blaze/blaze_app.c \
               $(SRC_DIR)/coding/vanta.c \
               $(SRC_DIR)/coding/vanta_examples.c \
               $(SRC_DIR)/kernel_panic/kpanic_compat.c \
               $(SRC_DIR)/kernel_panic/kpanic_core.c \
               $(SRC_DIR)/kernel_panic/kpanic_cpu.c \
               $(SRC_DIR)/kernel_panic/kpanic_emit.c \
               $(SRC_DIR)/kernel_panic/kpanic_halt.c \
               $(SRC_DIR)/kernel_panic/kpanic_log.c \
               $(SRC_DIR)/kernel_panic/kpanic_mem.c \
               $(SRC_DIR)/kernel_panic/kpanic_subsys.c \
               $(SRC_DIR)/kernel_panic/kpanic_trace.c \
               AurionGL/auriongl.c

# Object files — preserve subdirectory structure under OBJ_DIR
ASM_OBJS      := $(patsubst $(SRC_DIR)/asm/%.asm,$(OBJ_DIR)/asm/%.o,$(ASM_KERNEL))
C_OBJS        := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(filter $(SRC_DIR)/%.c,$(C_SOURCES)))
C_OBJS        += $(patsubst %.c,$(OBJ_DIR)/%.o,$(filter-out $(SRC_DIR)/%.c,$(C_SOURCES)))
ALL_OBJS      := $(ASM_OBJS) $(C_OBJS)

# Build Rules

.PHONY: all all-debug
all: check-platform check-build-type $(FLOPPY_IMG) $(ISO_IMG) $(HDD_IMG)
	@echo "======================================"
	@echo "AurionOS build completed successfully!"
	@echo "Floppy: $(FLOPPY_IMG)"
	@echo "ISO:    $(ISO_IMG)"
	@echo "HDD:    $(HDD_IMG) (persistent storage)"
	@echo ""
	@echo "IMPORTANT: For persistent installation:"
	@echo "  1. First boot: Use ISO to install"
	@echo "  2. After install: Boot from HDD for persistence"
	@echo "  VMware: Add both ISO (CD-ROM) and HDD as hard disk"
	@echo "  VirtualBox: Add both ISO and HDD, set boot order"
	@echo "======================================"

# Check if we need to clean due to build type change
.PHONY: check-build-type
check-build-type:
	@if [ -f $(BUILD_DIR)/.debug_build ]; then \
		echo "Switching from debug to release build - cleaning objects..."; \
		$(MAKE) clean-objs; \
		$(RM) $(BUILD_DIR)/.debug_build; \
	fi

.PHONY: check-platform
check-platform:
	@mkdir -p $(BUILD_DIR) || true
	@if [ -f $(BUILD_DIR)/.platform_tag ]; then \
		prev=$$(cat $(BUILD_DIR)/.platform_tag); \
		if [ "$$prev" != "$(PLATFORM_TAG)" ]; then \
			echo "Platform/toolchain changed ($$prev -> $(PLATFORM_TAG)) - cleaning objects..."; \
			$(MAKE) clean-objs; \
		fi; \
	fi
	@echo "$(PLATFORM_TAG)" > $(BUILD_DIR)/.platform_tag

.PHONY: clean-objs
clean-objs:
	@echo "Cleaning object files..."
	@rm -rf $(OBJ_DIR) 2>/dev/null || true
	@$(RM) $(call pathfix,$(KERNEL_BIN)) 2>/dev/null || true
	@echo "Object clean complete"

# Debug build with pre-installed system (skips installer)
all-debug: CFLAGS += -DDEBUG_SKIP_INSTALL
all-debug: check-platform check-debug-type clean-objs
	@$(MAKE) $(KERNEL_BIN) $(BOOT_BIN) CFLAGS="$(CFLAGS) -DDEBUG_SKIP_INSTALL"
	@touch $(BUILD_DIR)/.debug_build
	@echo "======================================"
	@echo "Building debug version..."
	@echo "======================================"
	@$(PYTHON) tools/mkfloppy.py $(BUILD_DIR)/aurionos_debug.img $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@echo "INSTALLED" > $(BUILD_DIR)/installed_marker.tmp
	@$(PYTHON) -c "import sys; sys.path.insert(0, 'tools'); from mkfloppy import add_file_to_floppy; add_file_to_floppy('$(BUILD_DIR)/aurionos_debug.img', '/installed.sys', '$(BUILD_DIR)/installed_marker.tmp')" 2>/dev/null || true
	@$(RM) $(BUILD_DIR)/installed_marker.tmp 2>/dev/null || true
	@$(PYTHON) tools/mkiso.py $(BUILD_DIR)/aurionos_debug.iso $(BUILD_DIR)/aurionos_debug.img $(WALLPAPER_0)
	@cp -f $(BUILD_DIR)/aurionos_debug.iso v86-web/aurionos_debug.iso && echo "✓ Copied ISO → v86-web/aurionos_debug.iso (browser demo)"
	@echo ""
	@echo "======================================"
	@echo "Debug build complete!"

.PHONY: check-debug-type
check-debug-type:
	@if [ ! -f $(BUILD_DIR)/.debug_build ]; then \
		echo "Switching to debug build - cleaning objects..."; \
	fi
	@echo "======================================"
	@echo "Debug IMG: $(BUILD_DIR)/aurionos_debug.img"
	@echo "Debug ISO: $(BUILD_DIR)/aurionos_debug.iso"
	@echo ""
	@echo "Installation screen is skipped."
	@echo ""
	@echo "To run in v86:"
	@echo "  1. Open v86-web/index.html in browser"
	@echo "  2. It will auto-load aurionos_debug.iso"
	@echo ""
	@echo "======================================"

# Directory Creation
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR) || true

$(OBJ_DIR): $(BUILD_DIR)
	@mkdir -p $(OBJ_DIR) || true
	@mkdir -p $(OBJ_DIR)/asm || true
	@mkdir -p $(OBJ_DIR)/core || true
	@mkdir -p $(OBJ_DIR)/gui || true
	@mkdir -p $(OBJ_DIR)/fs || true
	@mkdir -p $(OBJ_DIR)/net || true
	@mkdir -p $(OBJ_DIR)/crypto || true
	@mkdir -p $(OBJ_DIR)/apps || true
	@mkdir -p $(OBJ_DIR)/apps/Blaze || true
	@mkdir -p $(OBJ_DIR)/drivers || true
	@mkdir -p $(OBJ_DIR)/drivers/mp3 || true
	@mkdir -p $(OBJ_DIR)/coding || true
	@mkdir -p $(OBJ_DIR)/AurionGL || true

$(ISO_DIR):
	@$(MKDIR) $(ISO_DIR)

# Stage2 Build (must exist before the boot sector: its size sets KERNEL_LBA)
$(STAGE2_BIN): $(STAGE2_SRC) | $(BUILD_DIR)
	@echo "Assembling stage2: $<"
	@$(NASM) $(NASMFLAGS_BIN) -DKERNEL_DEST=0x100000 $< -o $@

# Bootloader Build
$(BOOT_BIN): $(ASM_BOOT) $(STAGE2_BIN) $(KERNEL_BIN) tools/build_bootloader.py | $(BUILD_DIR)
	@echo "Assembling bootloader: $<"
	@$(PYTHON) tools/build_bootloader.py $(NASM) "$(NASMFLAGS_BIN)" $< $@

# AP trampoline raw image (must exist before smp_low.o assembles incbin)
$(TRAMP_BIN): $(TRAMP_SRC) | $(BUILD_DIR)
	@echo "Assembling AP trampoline: $<"
	@$(NASM) $(NASMFLAGS_BIN) $< -o $@

# Pattern rule: assemble any .asm from src/asm/
# smp_low.o embeds the trampoline image, so it waits for the raw binary.
$(OBJ_DIR)/asm/smp_low.o: $(SRC_DIR)/asm/smp_low.asm $(TRAMP_BIN) | $(OBJ_DIR)
	@mkdir -p $(dir $@) || true
	@echo "Assembling: $<"
	@$(NASM) $(NASMFLAGS_ELF) $< -o $@
$(OBJ_DIR)/asm/%.o: $(SRC_DIR)/asm/%.asm | $(OBJ_DIR)
	@mkdir -p $(dir $@) || true
	@echo "Assembling: $<"
	@$(NASM) $(NASMFLAGS_ELF) $< -o $@

# Pattern rule: compile any .c from src/ subdirectories
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@) || true
	@echo "Compiling: $<"
	@$(GCC) $(CFLAGS) $< -o $@

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	@mkdir -p $(dir $@) || true
	@echo "Compiling: $<"
	@$(GCC) $(CFLAGS) -Iinclude $< -o $@

# Kernel Linking (with Rust library if available)
$(KERNEL_BIN): $(ALL_OBJS) link.ld | $(BUILD_DIR)
	@echo "Linking kernel..."
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS) $(LIBGCC)
	@echo "✓ Kernel linked successfully"
	@$(PYTHON) -c "import os; sz=os.path.getsize('$@'); print(f'Kernel size: {sz} bytes ({sz//1024}K)' )"
	@$(PYTHON) -c "import os,sys; sys.exit(0 if os.path.getsize('$@') <= 983040 else 1)" || (echo "ERROR: kernel exceeds 960KiB and would collide with BSS at 0x200000"; exit 1)

# Floppy Image Assembly (for QEMU)
$(FLOPPY_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN) tools/mkfloppy.py
	@echo "Creating floppy image..."
	@$(PYTHON) tools/mkfloppy.py $@ $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@echo "✓ Floppy image created: $@"

# Default wallpapers must exist in the tree (see tools/prepare_png_assets.py).
$(WALLPAPER_PNGS):
	@if [ ! -f $@ ]; then echo "Error: $@ missing (run tools/prepare_png_assets.py)"; exit 1; fi

# ISO Image Assembly
# Uses tools/mkiso.py to build a hybrid ISO 9660 image.
# El Torito no-emulation boot - compatible with VirtualBox, VMware, QEMU, real hardware.
$(ISO_IMG): $(FLOPPY_IMG) $(HDD_IMG) $(WALLPAPER_PNGS) tools/mkiso.py | $(BUILD_DIR)
	@echo "Creating hybrid ISO image..."
	@$(PYTHON) tools/mkiso.py $@ $(FLOPPY_IMG) $(WALLPAPER_0)
	@$(PYTHON) tools/embed_icons.py $@ icons
	@echo "✓ ISO image created: $@"

# Running and Testing

# Embed icons into HDD image starting at LBA 20000
.PHONY: embed-icons
embed-icons: $(HDD_IMG)
	@echo "Embedding system icons into HDD image..."
	@$(PYTHON) tools/embed_icons.py $(HDD_IMG) icons

# Create persistent hard disk image (empty - no pre-installation)
# Wallpaper/icon LBA layout must match src/gui/window_manager.c + icon_loader.c:
#   10000 Night_ridge.png / 20000 Misty_conife.png / 40000 Gate_B14_217_AM.png / 50000 Sunday_4_47_PM.png / 30000 ins.png / 60000+ icons
$(HDD_IMG): $(FLOPPY_IMG) $(WALLPAPER_PNGS) tools/mkfloppy.py | $(BUILD_DIR)
	@if [ ! -f $@ ]; then \
		echo "Creating empty HDD image ($(HDD_SIZE_MB)MB)..."; \
		$(DD) if=/dev/zero of=$@ bs=1M count=$(HDD_SIZE_MB) status=progress; \
		$(DD) if=$(FLOPPY_IMG) of=$@ bs=512 conv=notrunc status=none; \
		echo "✓ Empty HDD created: $@"; \
		$(PYTHON) tools/embed_wallpaper.py $@ $(WALLPAPER_0) 10000 2>/dev/null || echo "WARNING: Wallpaper 0 embedding failed"; \
		$(PYTHON) tools/embed_wallpaper.py $@ $(WALLPAPER_1) 20000 2>/dev/null || echo "WARNING: Wallpaper 1 embedding failed"; \
		$(PYTHON) tools/embed_wallpaper.py $@ $(WALLPAPER_2) 40000 2>/dev/null || echo "WARNING: Wallpaper 2 embedding failed"; \
		$(PYTHON) tools/embed_wallpaper.py $@ $(WALLPAPER_3) 50000 2>/dev/null || echo "WARNING: Wallpaper 3 embedding failed"; \
		$(PYTHON) tools/embed_wallpaper.py $@ $(WALLPAPER_INS) 30000 2>/dev/null || echo "WARNING: Installer wallpaper embedding failed"; \
		$(PYTHON) tools/embed_icons.py $@ icons || echo "WARNING: Icon embedding failed"; \
	else \
		echo "✓ HDD image preserved: $@ (delete to recreate)"; \
	fi

.PHONY: run
run: $(FLOPPY_IMG) $(HDD_IMG)
	@echo "Starting QEMU with floppy + HDD..."
	@if [ "$(IS_WSL)" = "yes" ]; then \
		FLP_WIN=$$(wslpath -w $(FLOPPY_IMG)); \
		HDD_WIN=$$(wslpath -w $(HDD_IMG)); \
		QEMU_WIN="/mnt/c/Program Files/qemu/qemu-system-i386.exe"; \
		if [ -x "$$QEMU_WIN" ]; then \
			"$$QEMU_WIN" \
				-drive "file=$$FLP_WIN,if=floppy,format=raw" \
				-drive "file=$$HDD_WIN,if=ide,format=raw" \
				-boot a -m 512M -smp 1 \
				-vga vmware; \
		else \
			$(QEMU) -drive file=$(FLOPPY_IMG),if=floppy,format=raw \
				-drive file=$(HDD_IMG),if=ide,format=raw \
				-boot a -m 512M -smp 1 \
				-vga vmware; \
		fi; \
	else \
	$(QEMU) -drive file=$(FLOPPY_IMG),if=floppy,format=raw \
		-drive file=$(HDD_IMG),if=ide,format=raw \
		-boot a -m 512M -smp 1 \
		-vga vmware; \
	fi

.PHONY: run-dbg
run-dbg: $(FLOPPY_IMG) $(HDD_IMG)
	@echo "Starting QEMU in debug display mode..."
	@$(QEMU) -drive file=$(FLOPPY_IMG),if=floppy,format=raw \
		-drive file=$(HDD_IMG),if=ide,format=raw \
		-boot a -m 512M -smp 1 \
		-vga vmware \
		-display sdl

.PHONY: run-debug
run-debug: $(FLOPPY_IMG) $(HDD_IMG)
	@echo "Starting QEMU with debug options..."
	@$(QEMU) -drive file=$(FLOPPY_IMG),if=floppy,format=raw \
		-drive file=$(HDD_IMG),if=ide,format=raw \
		-boot a -m 512M -smp 1 \
		-vga vmware \
		-d int,cpu_reset -no-reboot

.PHONY: run-iso
run-iso: $(ISO_IMG) $(HDD_IMG)
	@echo "Starting QEMU with ISO (CD-ROM Mode)..."
	@if [ ! -f $(ISO_IMG) ]; then \
		echo "ERROR: ISO file not found at $(ISO_IMG)"; \
		exit 1; \
	fi
	@echo "Booting ISO as CD-ROM (Drive D)..."
	@if [ "$(IS_WSL)" = "yes" ]; then \
		ISO_WIN=$$(wslpath -w $(ISO_IMG)); \
		HDD_WIN=$$(wslpath -w $(HDD_IMG)); \
		QEMU_WIN="/mnt/c/Program Files/qemu/qemu-system-i386.exe"; \
		if [ -x "$$QEMU_WIN" ]; then \
		"$$QEMU_WIN" -cdrom "$$ISO_WIN" \
			-drive "file=$$HDD_WIN,if=ide,format=raw" \
			-boot order=d,strict=on -m 512M -smp 1 \
			-vga vmware \
			-no-reboot; \
	else \
		$(QEMU) -cdrom $(ISO_IMG) \
			-drive file=$(HDD_IMG),if=ide,format=raw \
			-boot order=d,strict=on -m 512M -smp 1 \
			-vga vmware \
			-no-reboot; \
		fi; \
	else \
	$(QEMU) -cdrom $(ISO_IMG) \
		-drive file=$(HDD_IMG),if=ide,format=raw \
		-boot order=d,strict=on -m 512M -smp 1 \
		-vga vmware \
		-no-reboot; \
	fi

# Reset the hard disk (clear all saved data)
.PHONY: reset-hdd
reset-hdd:
	@echo "Resetting hard disk image..."
	@$(RM) $(HDD_IMG)
	@$(DD) if=/dev/zero of=$(HDD_IMG) bs=1M count=$(HDD_SIZE_MB) status=none
	@echo "✓ Hard disk reset complete"

# Debugging and Analysis

.PHONY: dump-boot
dump-boot: $(BOOT_BIN)
	@echo "Bootloader hexdump:"
	@$(HEXDUMP) -C $@ | head -n 32

.PHONY: dump-kernel
dump-kernel: $(KERNEL_BIN)
	@echo "Kernel hexdump (first 512 bytes):"
	@$(HEXDUMP) -C $@ | head -n 32

.PHONY: info
info:
	@echo "======================================"
	@echo "AurionOS Build Information"
	@echo "======================================"
	@if [ -f $(BOOT_BIN) ]; then \
		echo "Bootloader:  $(BOOT_BIN)"; \
		echo "  Size:      $$(stat -c%s $(BOOT_BIN)) bytes"; \
		echo ""; \
	else \
		echo "Bootloader:  $(BOOT_BIN) [NOT BUILT]"; \
		echo ""; \
	fi
	@if [ -f $(KERNEL_BIN) ]; then \
		echo "Kernel:      $(KERNEL_BIN)"; \
		echo "  Size:      $$(stat -c%s $(KERNEL_BIN)) bytes"; \
		echo ""; \
	else \
		echo "Kernel:      $(KERNEL_BIN) [NOT BUILT]"; \
		echo ""; \
	fi
	@if [ -f $(FLOPPY_IMG) ]; then \
		echo "Floppy Image: $(FLOPPY_IMG)"; \
		echo "  Size:       $$(stat -c%s $(FLOPPY_IMG)) bytes"; \
		echo ""; \
	else \
		echo "Floppy Image: $(FLOPPY_IMG) [NOT BUILT]"; \
		echo ""; \
	fi
	@if [ -f $(ISO_IMG) ]; then \
		echo "ISO Image:    $(ISO_IMG)"; \
		echo "  Size:       $$(stat -c%s $(ISO_IMG)) bytes"; \
	else \
		echo "ISO Image:    $(ISO_IMG) [NOT BUILT]"; \
	fi
	@echo "======================================"

# Cleaning
.PHONY: clean
clean:
	@echo "Cleaning build artifacts (preserving HDD for persistence)..."
	@$(RM) $(call pathfix,$(BOOT_BIN)) $(call pathfix,$(KERNEL_BIN)) $(call pathfix,$(FLOPPY_IMG)) $(call pathfix,$(ISO_IMG))
	@echo "✓ Clean complete (HDD preserved at $(HDD_IMG))"

.PHONY: clean-all
clean-all:
	@echo "Cleaning ALL build artifacts including HDD..."
	@$(RMDIR) $(call pathfix,$(BUILD_DIR))
	@echo "✓ Full clean complete"



.PHONY: clean-img
clean-img:
	@echo "Cleaning disk images..."
	@$(RM) $(call pathfix,$(FLOPPY_IMG)) $(call pathfix,$(ISO_IMG))
	@$(RMDIR) $(call pathfix,$(ISO_DIR))
	@echo "✓ Disk images cleaned"

.PHONY: rebuild
rebuild: clean-all all
	@echo "Note: HDD was reset. All saved files cleared."

# Security regression audit (AUR-18..AUR-27)
# Fails if a fixed pattern is reintroduced. Run with: make audit
.PHONY: audit
audit:
	@echo "======================================"
	@echo "Security regression audit (AUR-18..27)"
	@echo "======================================"
	@echo "[1/5] Inline event handlers reintroduced in Blaze..."
	@if grep -Erq "onclick_script|onmouseover_script|onsubmit_script|oninput_script|onblur_script" src/apps/Blaze; then \
		echo "FAIL: event handler fields/parsing present"; exit 1; \
	else \
		echo "  OK: no inline event handler code"; \
	fi
	@echo "[2/5] Default password literal reintroduced..."
	@if grep -Erq 'store_password[^;]*"root"' src/core src/gui; then \
		echo "FAIL: default root password present"; exit 1; \
	else \
		echo "  OK: no default credential"; \
	fi
	@echo "[3/5] uint16 port accumulation wrap reintroduced..."
	@if grep -Erq '\*port = \*port \* 10' src/net; then \
		echo "FAIL: unguarded port accumulation"; exit 1; \
	else \
		echo "  OK: ports accumulate in guarded wide types"; \
	fi
	@echo "[4/5] Unbounded secret input (review queue, informational)..."
	@hits=$$(grep -Ern "password\[pi\+\+\]|gui_input_len\+\+" src/gui src/core | grep -v "PW_MAX" || true); \
	if [ -n "$$hits" ]; then \
		echo "  REVIEW QUEUE (inspect each):"; \
		echo "$$hits"; \
	else \
		echo "  OK: no unbounded secret grow-sites"; \
	fi
	@echo "[5/5] Legacy djb2 auth path reintroduced..."
	@if grep -Erq "legacy_djb2_hash\(|is_legacy_djb2_hash" src/core; then \
		echo "FAIL: legacy auth code present"; exit 1; \
	else \
		echo "  OK: legacy hashes refused by policy"; \
	fi
	@echo "======================================"
	@echo "Audit complete"

# Help
.PHONY: help
help:
	@echo "======================================"
	@echo "AurionOS Makefile - Available Targets"
	@echo "======================================"
	@echo "Building:"
	@echo "  make all          - Build complete system (default)"

	@echo "  make rebuild      - Clean and rebuild everything"
	@echo "  make clean        - Remove build artifacts (preserves HDD)"
	@echo "  make clean-all    - Remove ALL artifacts including HDD"

	@echo ""
	@echo "Running:"
	@echo "  make run          - Build and run in QEMU (floppy + HDD)"
	@echo "  make run-iso      - Build and run in QEMU (ISO + HDD)"
	@echo "  make run-debug    - Run with CPU debug output"
	@echo ""
	@echo "Storage:"
	@echo "  make reset-hdd    - Reset HDD (clear all saved files)"
	@echo "  HDD location: $(HDD_IMG)"
	@echo ""
	@echo "Info:"
	@echo "  make info         - Display build information"
	@echo "  make help         - Show this help message"
	@echo "======================================"
	@echo ""
	@echo "Filesystem Commands in AurionOS:"
	@echo "  TOUCH file  - Create empty file (persists after reboot)"
	@echo "  MKDIR dir   - Create directory (persists after reboot)"
	@echo "  NANO file   - Edit file (persists after reboot)"
	@echo "  CD dir      - Change directory (persists after reboot)"
	@echo "  DIR/LS      - List files in current directory"
	@echo "======================================"

# Dependencies
$(KERNEL_BIN): $(ALL_OBJS)
$(BOOT_BIN): $(ASM_BOOT) $(STAGE2_BIN)
$(FLOPPY_IMG): $(BOOT_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
$(ISO_IMG): $(FLOPPY_IMG)

.DEFAULT_GOAL := all

# Regenerate the embedded catalog whenever its source examples change.
$(SRC_DIR)/coding/vanta_examples.c: tools/embed_vanta_examples.py $(wildcard $(SRC_DIR)/coding/examples/*.vanta)
	$(PYTHON) tools/embed_vanta_examples.py

# Shared filesystem layout and bundled Vanta example declarations.
$(OBJ_DIR)/core/commands.o $(OBJ_DIR)/core/installer.o $(OBJ_DIR)/core/reinstall.o $(OBJ_DIR)/fs/fs_maintenance.o: $(SRC_DIR)/fs/fs_maintenance.h include/fs.h
$(OBJ_DIR)/core/commands.o $(OBJ_DIR)/core/installer.o $(OBJ_DIR)/coding/vanta_examples.o: $(SRC_DIR)/coding/vanta_examples.h
$(OBJ_DIR)/core/commands.o $(OBJ_DIR)/core/reinstall.o: $(SRC_DIR)/core/reinstall.h
$(OBJ_DIR)/gui/desktop.o: include/fs.h
