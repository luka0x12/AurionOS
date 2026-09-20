#!/usr/bin/env python3
"""
Embed a PNG wallpaper into an AurionOS HDD image at a known LBA offset.

Slots (must match src/gui/window_manager.c):
  10000: Night_ridge.png (desktop slot 0)
  20000: Misty_conife.png (desktop slot 1, default)
  40000: Gate_B14_217_AM.png (desktop slot 2)
  50000: Sunday_4_47_PM.png (desktop slot 3)
  30000: ins.png (installer background)
Each slot reserves 8192 sectors (4 MB).
"""
import sys
import os
import struct

SLOT_SECTORS = 8192

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def png_dims(path):
    with open(path, "rb") as f:
        sig = f.read(8)
        if sig != PNG_SIG:
            return None
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                return None
            ln = struct.unpack(">I", hdr[0:4])[0]
            typ = hdr[4:8]
            if typ == b"IHDR":
                data = f.read(13)
                w, h = struct.unpack(">II", data[0:8])
                return (w, h)
            f.seek(ln + 4, os.SEEK_CUR)


def main():
    if len(sys.argv) < 3:
        print("Usage: embed_wallpaper.py <hdd_image> <wallpaper.png> [lba]")
        sys.exit(1)

    hdd_path = sys.argv[1]
    png_path = sys.argv[2]

    if not os.path.exists(hdd_path):
        print(f"Error: HDD image not found: {hdd_path}")
        sys.exit(1)

    if not os.path.exists(png_path):
        print(f"Error: wallpaper not found: {png_path}")
        sys.exit(1)

    target_lba = 10000
    if len(sys.argv) >= 4:
        target_lba = int(sys.argv[3], 10)
        if target_lba <= 0:
            print(f"Error: invalid LBA: {target_lba}")
            sys.exit(1)

    with open(png_path, "rb") as f:
        png_data = f.read()

    if len(png_data) == 0:
        print("Error: wallpaper file is empty")
        sys.exit(1)

    if png_data[0:8] != PNG_SIG:
        print("Error: not a PNG file (bad signature)")
        sys.exit(1)

    dims = png_dims(png_path)
    print(f"Wallpaper: {png_path}")
    print(f"  Size: {len(png_data)} bytes ({len(png_data)//1024}KB)")
    if dims:
        print(f"  Dimensions: {dims[0]}x{dims[1]}")

    capacity = SLOT_SECTORS * 512
    if len(png_data) > capacity:
        print(f"Error: wallpaper ({len(png_data)} bytes) exceeds slot ({capacity} bytes)")
        sys.exit(1)

    hdd_size = os.path.getsize(hdd_path)
    target_offset = target_lba * 512
    if target_offset + capacity > hdd_size:
        print(f"Error: slot at LBA {target_lba} past end of HDD ({hdd_size} bytes)")
        sys.exit(1)

    with open(hdd_path, "r+b") as f:
        f.seek(target_offset)
        f.write(png_data)
        tail = capacity - len(png_data)
        if tail:
            f.write(b"\x00" * tail)

    print(f"Embedded at LBA {target_lba} (offset {target_offset})")


if __name__ == "__main__":
    main()
