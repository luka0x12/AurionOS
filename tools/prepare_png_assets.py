#!/usr/bin/env python3
"""Prepare AurionOS PNG assets from the New-Logos drop.

Reads icons/New-Logos/*.png (2880px masters), resizes to 128x128 dock
icons, and writes icons/*.png. Resizes Wallpaper/*.png masters to
1920x1080. Run once; the tree then carries only OS-sized PNGs.

Usage: python3 tools/prepare_png_assets.py [--check]
  --check: verify outputs exist and are sane, do not rewrite.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_DIR = os.path.join(ROOT, "icons", "New-Logos")
DST_DIR = os.path.join(ROOT, "icons")
WP_DIR = os.path.join(ROOT, "Wallpaper")

ICON_SIZE = 128
WP_SIZE = (1920, 1080)

ICON_MAP = {
    "3d_demo.png": "3d_demo.png",
    "audio_manager.png": "audio_manager.png",
    "Browser.png": "browser.png",
    "calculator.png": "calculator.png",
    "clock.png": "clock.png",
    "files.png": "files.png",
    "notepad.png": "notepad.png",
    "paint.png": "paint.png",
    "settings.png": "settings.png",
    "snake.png": "snake.png",
    "sys-info.png": "sys-info.png",
    "task_manager.png": "task_manager.png",
    "terminal.png": "terminal.png",
}

WALLPAPERS = ["Night_ridge.png", "Misty_conife.png", "Gate_B14_217_AM.png", "Sunday_4_47_PM.png", "ins.png"]
THUMB_SIZE = (144, 81)
THUMBS = {"Night_ridge.png": "thumb_night.png",
          "Misty_conife.png": "thumb_misty.png",
          "Gate_B14_217_AM.png": "thumb_gate.png",
          "Sunday_4_47_PM.png": "thumb_sunday.png"}


def save_retry(im, dst):
    import time
    last = None
    for _ in range(10):
        try:
            im.save(dst, optimize=True)
            return
        except OSError as e:
            last = e
            time.sleep(0.3)
    raise last


def need_pil():
    try:
        from PIL import Image
        return Image
    except ImportError:
        print("Error: Pillow required: pip install Pillow")
        sys.exit(1)


def key_out_background(im, tol=16, halo_tol=40):
    """Flood-fill the edge-connected backdrop to transparent.

    Logo masters ship as full-bleed opaque squares; the dock expects
    floating art with real alpha. The key is the mean of the four
    corners; 4-connected fill from every border pixel within tol eats
    the backdrop, then a 1px halo pass (halo_tol) clears fringe left
    by the downscale filter. Returns the replace count.
    """
    w, h = im.size
    px = im.load()
    key = [0, 0, 0]
    for cx, cy in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        r, g, b, a = px[cx, cy]
        key[0] += r
        key[1] += g
        key[2] += b
    key = [c // 4 for c in key]

    def near(p, t):
        r, g, b, a = p
        return (abs(r - key[0]) <= t and abs(g - key[1]) <= t and
                abs(b - key[2]) <= t)

    from collections import deque
    seen = bytearray(w * h)
    q = deque()
    for x in range(w):
        q.append((x, 0))
        q.append((x, h - 1))
    for y in range(h):
        q.append((0, y))
        q.append((w - 1, y))
    while q:
        x, y = q.popleft()
        if x < 0 or y < 0 or x >= w or y >= h:
            continue
        if seen[y * w + x]:
            continue
        seen[y * w + x] = 1
        if not near(px[x, y], tol):
            continue
        r, g, b, a = px[x, y]
        px[x, y] = (r, g, b, 0)
        q.append((x + 1, y))
        q.append((x - 1, y))
        q.append((x, y + 1))
        q.append((x, y - 1))
    # halo: neighbors of cleared pixels that still look like backdrop go too
    cleared = 0
    for y in range(h):
        for x in range(w):
            if px[x, y][3] != 0 or not near(px[x, y], halo_tol):
                continue
            adj = False
            if x > 0 and px[x - 1, y][3] == 0:
                adj = True
            elif x + 1 < w and px[x + 1, y][3] == 0:
                adj = True
            elif y > 0 and px[x, y - 1][3] == 0:
                adj = True
            elif y + 1 < h and px[x, y + 1][3] == 0:
                adj = True
            if adj:
                r, g, b, a = px[x, y]
                px[x, y] = (r, g, b, 0)
                cleared += 1
    return cleared


def build_icons(Image):
    missing = []
    if not os.path.isdir(SRC_DIR):
        # Masters were consumed (deleted after import); just re-key the
        # shipped 128px icons in place.
        for dst_name in list(ICON_MAP.values()):
            dst = os.path.join(DST_DIR, dst_name)
            if not os.path.exists(dst):
                missing.append(dst_name)
                continue
            with Image.open(dst) as src_im:
                src_im.load()
                im = src_im.convert("RGBA")
            if im.size != (ICON_SIZE, ICON_SIZE):
                im = im.resize((ICON_SIZE, ICON_SIZE), Image.Resampling.LANCZOS)
            n = key_out_background(im)
            save_retry(im, dst)
            print("icon (rekey) %s (%d bytes, keyed %d)" % (dst_name, os.path.getsize(dst), n))
        if missing:
            print("MISSING icons: %s" % ", ".join(missing))
            sys.exit(1)
        return
    for src_name, dst_name in ICON_MAP.items():
        src = os.path.join(SRC_DIR, src_name)
        dst = os.path.join(DST_DIR, dst_name)
        if not os.path.exists(src):
            missing.append(src_name)
            continue
        with Image.open(src) as src_im:
            src_im.load()
            im = src_im.convert("RGBA")
        if im.size != (ICON_SIZE, ICON_SIZE):
            im = im.resize((ICON_SIZE, ICON_SIZE), Image.Resampling.LANCZOS)
        n = key_out_background(im)
        save_retry(im, dst)
        print("icon %s -> %s (%d bytes, keyed %d)" % (src_name, dst_name, os.path.getsize(dst), n))
    # folder.png + file.png have no New-Logos master; draw flat glyphs so no
    # slot falls back to a diagnostic color square.
    draw_folder(Image, os.path.join(DST_DIR, "folder.png"))
    draw_file(Image, os.path.join(DST_DIR, "file.png"))
    if missing:
        print("MISSING masters: %s" % ", ".join(missing))
        sys.exit(1)


def draw_folder(Image, dst):
    from PIL import ImageDraw
    im = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    s = ICON_SIZE
    dr.rounded_rectangle([s*0.08, s*0.22, s*0.42, s*0.34], radius=s*0.04, fill=(255, 187, 51, 255))
    dr.rounded_rectangle([s*0.06, s*0.30, s*0.94, s*0.86], radius=s*0.06, fill=(255, 204, 68, 255))
    dr.rectangle([s*0.06, s*0.30, s*0.94, s*0.38], fill=(255, 221, 85, 255))
    save_retry(im, dst)
    print("icon (drawn) -> folder.png (%d bytes)" % os.path.getsize(dst))


def draw_file(Image, dst):
    from PIL import ImageDraw
    im = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    s = ICON_SIZE
    dr.rectangle([s*0.24, s*0.06, s*0.76, s*0.94], fill=(240, 240, 255, 255))
    dr.polygon([(s*0.76, s*0.06), (s*0.76, s*0.26), (s*0.56, s*0.06)], fill=(208, 208, 224, 255))
    for i, y in enumerate((0.40, 0.52, 0.64, 0.76)):
        x1 = s*0.32
        x2 = s*0.68 if i < 3 else s*0.58
        dr.rectangle([x1, s*y, x2, s*y + max(2, s*0.02)], fill=(128, 128, 144, 255))
    save_retry(im, dst)
    print("icon (drawn) -> file.png (%d bytes)" % os.path.getsize(dst))


def build_wallpapers(Image):
    for name in WALLPAPERS:
        p = os.path.join(WP_DIR, name)
        if not os.path.exists(p):
            print("MISSING wallpaper: %s" % p)
            sys.exit(1)
        with Image.open(p) as src:
            src.load()
            im = src.convert("RGB")
        if im.size != WP_SIZE:
            im = im.resize(WP_SIZE, Image.Resampling.LANCZOS)
            tmp = p + ".tmp.png"
            save_retry(im, tmp)
            os.replace(tmp, p)
            print("wallpaper resized %s -> %dx%d (%d bytes)" % (name, WP_SIZE[0], WP_SIZE[1], os.path.getsize(p)))
        else:
            print("wallpaper ok %s (%d bytes)" % (name, os.path.getsize(p)))
        if name in THUMBS:
            tp = os.path.join(WP_DIR, THUMBS[name])
            th = im.resize(THUMB_SIZE, Image.Resampling.LANCZOS)
            save_retry(th, tp)
            print("thumbnail %s (%d bytes)" % (THUMBS[name], os.path.getsize(tp)))


def check():
    ok = True
    for dst_name in list(ICON_MAP.values()) + ["folder.png", "file.png"]:
        p = os.path.join(DST_DIR, dst_name)
        if not os.path.exists(p):
            print("MISSING %s" % p)
            ok = False
    for name in WALLPAPERS + list(THUMBS.values()):
        p = os.path.join(WP_DIR, name)
        if not os.path.exists(p):
            print("MISSING %s" % p)
            ok = False
    # no BMP/RLE stragglers, no New-Logos dir
    for root, dirs, files in os.walk(DST_DIR):
        for f in files:
            if f.lower().endswith((".bmp", ".rle")):
                print("STRAGGLER %s" % os.path.join(root, f))
                ok = False
    if os.path.isdir(SRC_DIR):
        print("STILL PRESENT: %s" % SRC_DIR)
        ok = False
    print("assets check: %s" % ("OK" if ok else "FAIL"))
    return ok


def main():
    Image = need_pil()
    if "--check" in sys.argv:
        sys.exit(0 if check() else 1)
    build_icons(Image)
    build_wallpapers(Image)
    print("done.")


if __name__ == "__main__":
    main()
