# Build bootloader with correct sector counts based on stage2/kernel sizes.
#
# Layout on disk: LBA 0 = 512-byte boot sector, LBA 1.. = stage2 image
# (real-mode BIOS services, whole 512-byte sectors), then the kernel,
# which the boot sector copies to 1MB through a bounce buffer.

import sys
import subprocess
import os


def sectors(path):
    return (os.path.getsize(path) + 511) // 512


def main():
    if len(sys.argv) != 5:
        print("Usage: python build_bootloader.py <nasm> <nasmflags> <boot_asm> <output_bin>")
        sys.exit(1)

    nasm = sys.argv[1]
    boot_asm = sys.argv[3]
    output_bin = sys.argv[4]
    stage2_bin = "build/stage2.bin"
    kernel_bin = "build/kernel.bin"

    # Stage2 must already exist (Makefile builds it first via its own rule).
    if not os.path.exists(stage2_bin):
        print(f"Error: {stage2_bin} not found, build stage2 first")
        sys.exit(1)
    stage2_sectors = sectors(stage2_bin)
    print(f"Stage2 size: {os.path.getsize(stage2_bin)} bytes ({stage2_sectors} sectors)")

    # Calculate kernel sectors and its LBA (right after stage2).
    if os.path.exists(kernel_bin):
        kernel_size = os.path.getsize(kernel_bin)
        kernel_sectors = sectors(kernel_bin)
        print(f"Kernel size: {kernel_size} bytes ({kernel_sectors} sectors)")
    else:
        print(f"Warning: {kernel_bin} not found, using default 64 sectors")
        kernel_sectors = 64
    kernel_lba = 1 + stage2_sectors
    print(f"Kernel LBA: {kernel_lba}")

    # Assemble bootloader.
    cmd = [
        nasm,
        "-f", "bin",
        f"-DSTAGE2_SECTORS={stage2_sectors}",
        f"-DKERNEL_LBA={kernel_lba}",
        f"-DKERNEL_SECTORS={kernel_sectors}",
        "-DKERNEL_DEST=0x100000",
        boot_asm,
        "-o", output_bin
    ]

    cmd_str = ' '.join(cmd)
    print(f"Assembling: {cmd_str}")
    result = subprocess.run(cmd_str, shell=True)

    if result.returncode != 0:
        print("ERROR: Assembly failed!")
        sys.exit(1)

    # Verify size
    boot_size = os.path.getsize(output_bin)
    if boot_size != 512:
        print(f"ERROR: Bootloader must be exactly 512 bytes, got {boot_size}!")
        sys.exit(1)

    print(f"✓ Bootloader built successfully (512 bytes)")

if __name__ == "__main__":
    main()
