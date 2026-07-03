from __future__ import annotations

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
CONTENT_DIR = ROOT / "server" / "content"

GAMES = [
    ("Homeworld", "homeworld", "HOMEWORLD", [(20, 30, 70), (180, 120, 30)]),
    ("AgeOfEmpires2", "aoe2", "AGE OF EMPIRES II", [(70, 45, 20), (190, 150, 70)]),
    ("DeusEx", "deusex", "DEUS EX", [(15, 45, 35), (210, 180, 60)]),
    ("Diablo2", "diablo2", "DIABLO II", [(55, 0, 0), (210, 40, 20)]),
    ("Half-Life", "halflife", "HALF-LIFE", [(70, 35, 0), (245, 130, 20)]),
    ("NeedForSpeedPorsche", "nfspu", "NFS: PORSCHE", [(20, 20, 25), (200, 30, 30)]),
    ("Quake3Arena", "q3a", "QUAKE III ARENA", [(40, 0, 0), (220, 65, 30)]),
    ("StarCraft", "starcraft", "STARCRAFT", [(10, 20, 60), (80, 180, 220)]),
    ("TheSims", "sims", "THE SIMS", [(25, 80, 35), (120, 220, 100)]),
    ("UnrealTournament", "ut99", "UNREAL TOURNAMENT", [(25, 20, 60), (210, 150, 40)]),
]

SCENES = ["TITLE", "GAMEPLAY", "DETAIL"]


def font(size: int):
    for name in ("arial.ttf", "Arial.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def draw_center(draw: ImageDraw.ImageDraw, xy: tuple[int, int], text: str, fnt, fill):
    bbox = draw.textbbox((0, 0), text, font=fnt)
    x = xy[0] - (bbox[2] - bbox[0]) // 2
    y = xy[1] - (bbox[3] - bbox[1]) // 2
    draw.text((x, y), text, font=fnt, fill=fill)


def make_image(title: str, scene: str, index: int, colors: list[tuple[int, int, int]]) -> Image.Image:
    img = Image.new("RGB", (320, 240), colors[0])
    draw = ImageDraw.Draw(img)
    c1, c2 = colors
    # simple gradient
    for y in range(240):
        t = y / 239
        col = tuple(int(c1[i] * (1 - t) + c2[i] * t) for i in range(3))
        draw.line((0, y, 319, y), fill=col)
    # 4:3 safe border and fake CRT scanlines/geometric content
    draw.rectangle((5, 5, 314, 234), outline=(245, 245, 220), width=2)
    draw.rectangle((12, 12, 307, 227), outline=(0, 0, 0), width=1)
    for y in range(18, 228, 8):
        draw.line((14, y, 305, y), fill=(0, 0, 0), width=1)
    accent = tuple(min(255, x + 70) for x in c2)
    if index == 1:
        draw.ellipse((35, 55, 115, 135), outline=accent, width=5)
        draw.polygon([(210, 55), (270, 120), (185, 135)], outline=accent, fill=None)
    elif index == 2:
        for x in range(35, 285, 45):
            draw.rectangle((x, 70 + (x % 3) * 12, x + 25, 145), outline=accent, width=3)
        draw.line((35, 170, 285, 105), fill=accent, width=4)
    else:
        for r in range(0, 90, 18):
            draw.arc((70-r//2, 45-r//2, 250+r//2, 185+r//2), 20, 340, fill=accent, width=3)
        draw.rectangle((70, 150, 250, 180), outline=accent, width=4)
    title_font = font(28 if len(title) < 12 else 22)
    scene_font = font(20)
    small_font = font(12)
    draw.rectangle((0, 0, 320, 42), fill=(0, 0, 0))
    draw_center(draw, (160, 22), title, title_font, (255, 255, 220))
    draw.rectangle((40, 192, 280, 224), fill=(0, 0, 0))
    draw_center(draw, (160, 208), f"P3 PLACEHOLDER {scene}", scene_font, (255, 255, 255))
    draw.text((10, 224), f"CYD Banners test asset #{index}", font=small_font, fill=(230, 230, 230))
    return img


def main() -> None:
    made = 0
    for folder, prefix, title, colors in GAMES:
        out_dir = CONTENT_DIR / "Pentium3-1ghz" / folder / "images"
        out_dir.mkdir(parents=True, exist_ok=True)
        for i, scene in enumerate(SCENES, 1):
            path = out_dir / f"{prefix}_{i}.jpg"
            img = make_image(title, scene, i, colors)
            img.save(path, quality=92)
            made += 1
            print(path.relative_to(CONTENT_DIR).as_posix())
    print(f"Generated placeholder JPGs: {made}")


if __name__ == "__main__":
    main()
