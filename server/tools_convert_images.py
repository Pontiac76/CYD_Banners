from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

try:
    from PIL import Image, ImageEnhance, ImageOps
except ImportError as exc:  # pragma: no cover - user-facing CLI message
    raise SystemExit("Pillow is required. Run: pip install -r server/requirements.txt") from exc

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTENT_DIR = ROOT / "server" / "content"
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png"}
DEFAULT_WIDTH = 320
DEFAULT_HEIGHT = 240
MAGIC = b"CYDIMG1\0"


def rgb888_to_rgb565_le(r: int, g: int, b: int) -> bytes:
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return struct.pack("<H", value)


def source_md5(source: Path) -> str:
    digest = hashlib.md5()
    with source.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 64), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_meta(
    source: Path,
    width: int,
    height: int,
    mode: str,
    auto_contrast: bool,
    contrast: float,
    brightness: float,
    saturation: float,
    gamma: float,
    dither: bool,
) -> dict[str, object]:
    return {
        "source": source.name,
        "source_md5": source_md5(source),
        "width": width,
        "height": height,
        "mode": mode,
        "auto_contrast": auto_contrast,
        "contrast": contrast,
        "brightness": brightness,
        "saturation": saturation,
        "gamma": gamma,
        "dither": dither,
        "format": "CYDIMG1_RGB565_LE",
    }


def meta_matches(meta_path: Path, expected: dict[str, object]) -> bool:
    try:
        return meta_path.exists() and json.loads(meta_path.read_text(encoding="utf-8")) == expected
    except Exception:
        return False


def apply_gamma(image: Image.Image, gamma: float) -> Image.Image:
    if gamma == 1.0:
        return image
    inv_gamma = 1.0 / gamma
    table = [max(0, min(255, round(((value / 255.0) ** inv_gamma) * 255.0))) for value in range(256)]
    return image.point(table * 3)


def quantize_channel(value: float, bits: int) -> int:
    max_value = (1 << bits) - 1
    return round(max(0.0, min(255.0, value)) * max_value / 255.0) * 255 // max_value


def iter_rgb565_pixels(image: Image.Image, dither: bool):
    if not dither:
        yield from image.getdata()
        return

    width, height = image.size
    pixels = [[list(pixel) for pixel in image.crop((0, y, width, y + 1)).getdata()] for y in range(height)]
    for y in range(height):
        for x in range(width):
            old = pixels[y][x]
            new = [quantize_channel(old[0], 5), quantize_channel(old[1], 6), quantize_channel(old[2], 5)]
            error = [old[channel] - new[channel] for channel in range(3)]
            pixels[y][x] = new
            for dx, dy, weight in ((1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)):
                nx = x + dx
                ny = y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    for channel in range(3):
                        pixels[ny][nx][channel] += error[channel] * weight
            yield (int(new[0]), int(new[1]), int(new[2]))


def convert_image(
    source: Path,
    dest: Path,
    width: int,
    height: int,
    mode: str,
    auto_contrast: bool,
    contrast: float,
    brightness: float,
    saturation: float,
    gamma: float,
    dither: bool,
) -> None:
    with Image.open(source) as image:
        image = image.convert("RGB")
        if mode == "fit":
            image = ImageOps.contain(image, (width, height), Image.Resampling.LANCZOS)
            canvas = Image.new("RGB", (width, height), (0, 0, 0))
            x = (width - image.width) // 2
            y = (height - image.height) // 2
            canvas.paste(image, (x, y))
            image = canvas
        else:
            image = ImageOps.fit(image, (width, height), Image.Resampling.LANCZOS, centering=(0.5, 0.5))

        if auto_contrast:
            image = ImageOps.autocontrast(image)
        if brightness != 1.0:
            image = ImageEnhance.Brightness(image).enhance(brightness)
        if contrast != 1.0:
            image = ImageEnhance.Contrast(image).enhance(contrast)
        if saturation != 1.0:
            image = ImageEnhance.Color(image).enhance(saturation)
        image = apply_gamma(image, gamma)

        dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open("wb") as handle:
            # Simple CYD-ready image format:
            # 8 bytes magic, uint16 little-endian width, uint16 little-endian height,
            # then width*height pixels as little-endian RGB565, row-major top-to-bottom.
            handle.write(MAGIC)
            handle.write(struct.pack("<HH", width, height))
            for r, g, b in iter_rgb565_pixels(image, dither):
                handle.write(rgb888_to_rgb565_le(r, g, b))


def iter_source_images(content_dir: Path) -> list[Path]:
    return sorted(
        (path for path in content_dir.rglob("*") if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS),
        key=lambda p: p.relative_to(content_dir).as_posix().lower(),
    )


def convert_all(
    content_dir: Path,
    width: int,
    height: int,
    mode: str,
    force: bool,
    auto_contrast: bool = False,
    contrast: float = 1.0,
    brightness: float = 1.0,
    saturation: float = 1.0,
    gamma: float = 1.0,
    dither: bool = False,
) -> int:
    count = 0
    skipped = 0
    for source in iter_source_images(content_dir):
        dest = source.with_suffix(".cyd")
        meta_path = dest.with_suffix(dest.suffix + ".meta.json")
        meta = expected_meta(source, width, height, mode, auto_contrast, contrast, brightness, saturation, gamma, dither)
        if not force and dest.exists() and meta_matches(meta_path, meta):
            skipped += 1
            continue
        convert_image(source, dest, width, height, mode, auto_contrast, contrast, brightness, saturation, gamma, dither)
        meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        count += 1
        adjustments = []
        if auto_contrast:
            adjustments.append("auto-contrast")
        if brightness != 1.0:
            adjustments.append(f"brightness={brightness:g}")
        if contrast != 1.0:
            adjustments.append(f"contrast={contrast:g}")
        if saturation != 1.0:
            adjustments.append(f"saturation={saturation:g}")
        if gamma != 1.0:
            adjustments.append(f"gamma={gamma:g}")
        if dither:
            adjustments.append("dither")
        adjustment_text = ", " + ", ".join(adjustments) if adjustments else ""
        print(f"Converted {source.relative_to(content_dir).as_posix()} -> {dest.relative_to(content_dir).as_posix()} ({mode} {width}x{height}{adjustment_text})")
    print(f"Image conversion complete: converted {count}, skipped {skipped}")
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert JPG/PNG files into CYD RGB565 .cyd files.")
    parser.add_argument("content_dir", nargs="?", default=str(DEFAULT_CONTENT_DIR), help="Published content directory")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--mode", choices=("cover", "fit"), default="fit", help="fit letterboxes and preserves full image; cover crops to fill")
    parser.add_argument("--force", action="store_true", help="regenerate even if .cyd is newer than source")
    parser.add_argument("--auto-contrast", action="store_true", help="stretch each image's color range before RGB565 conversion")
    parser.add_argument("--contrast", type=float, default=1.0, help="contrast multiplier; 1.0 leaves contrast unchanged")
    parser.add_argument("--brightness", type=float, default=1.0, help="brightness multiplier; 1.0 leaves brightness unchanged")
    parser.add_argument("--saturation", type=float, default=1.0, help="color saturation multiplier; 1.0 leaves saturation unchanged")
    parser.add_argument("--gamma", type=float, default=1.0, help="gamma correction; values below 1 darken midtones, above 1 brighten midtones")
    parser.add_argument("--dither", action="store_true", help="apply Floyd-Steinberg dithering when reducing to RGB565")
    args = parser.parse_args()
    convert_all(
        Path(args.content_dir).resolve(),
        args.width,
        args.height,
        args.mode,
        args.force,
        args.auto_contrast,
        args.contrast,
        args.brightness,
        args.saturation,
        args.gamma,
        args.dither,
    )


if __name__ == "__main__":
    main()
