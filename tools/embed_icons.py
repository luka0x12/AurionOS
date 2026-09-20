import sys
import os

# Order MUST match icons_load_all() in src/gui/icon_loader.c:
# index -> raw LBA 60000 + index*2000.
ICONS = [
    "terminal.png",
    "browser.png",
    "notepad.png",
    "calculator.png",
    "files.png",
    "clock.png",
    "paint.png",
    "sys-info.png",
    "settings.png",
    "folder.png",
    "file.png",
    "snake.png",
    "3d_demo.png",
    "task_manager.png",
    "audio_manager.png",
    "blockforge.png",
]

PNG_SIG = b"\x89PNG\r\n\x1a\n"

# Starting LBA (60000 = ~30MB mark, after the wallpaper slots)
START_LBA = 60000
LBA_OFFSET = 2000  # 1MB spacing (2000 * 512 = 1,024,000 bytes)


def embed_icons(img_path, icons_dir):
    with open(img_path, "r+b") as f:
        for i, name in enumerate(ICONS):
            path = os.path.join(icons_dir, name)
            if not os.path.exists(path):
                print(f"Skipping {name} - not found")
                continue

            with open(path, "rb") as icon_f:
                data = icon_f.read()

            if data[0:8] != PNG_SIG:
                print(f"Skipping {name} - not a PNG file")
                continue

            if len(data) > LBA_OFFSET * 512:
                print(f"Error: {name} ({len(data)} bytes) exceeds 1MB slot")
                sys.exit(1)

            f.seek(START_LBA * 512 + i * LBA_OFFSET * 512)
            f.write(data)
            # Zero the tail of the slot so no stale bytes follow IEND.
            tail = LBA_OFFSET * 512 - len(data)
            if tail:
                f.write(b"\x00" * tail)
            print(f"Embedded {name} at LBA {START_LBA + i * LBA_OFFSET}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: embed_icons.py <img_path> <icons_dir>")
        sys.exit(1)
    embed_icons(sys.argv[1], sys.argv[2])
