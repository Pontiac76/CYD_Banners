from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

try:
    from PIL import Image, ImageOps
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


def expected_meta(source: Path, width: int, height: int, mode: str) -> dict[str, object]:
    return {
        "source": source.name,
        "source_md5": source_md5(source),
        "width": width,
        "height": height,
        "mode": mode,
        "format": "CYDIMG1_RGB565_LE",
    }


def meta_matches(meta_path: Path, expected: dict[str, object]) -> bool:
    try:
        return meta_path.exists() and json.loads(meta_path.read_text(encoding="utf-8")) == expected
    except Exception:
        return False


def convert_image(source: Path, dest: Path, width: int, height: int, mode: str) -> None:
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

        dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open("wb") as handle:
            # Simple CYD-ready image format:
            # 8 bytes magic, uint16 little-endian width, uint16 little-endian height,
            # then width*height pixels as little-endian RGB565, row-major top-to-bottom.
            handle.write(MAGIC)
            handle.write(struct.pack("<HH", width, height))
            for r, g, b in image.getdata():
                handle.write(rgb888_to_rgb565_le(r, g, b))


def iter_source_images(content_dir: Path) -> list[Path]:
    return sorted(
        (path for path in content_dir.rglob("*") if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS),
        key=lambda p: p.relative_to(content_dir).as_posix().lower(),
    )


def convert_all(content_dir: Path, width: int, height: int, mode: str, force: bool) -> int:
    count = 0
    skipped = 0
    for source in iter_source_images(content_dir):
        dest = source.with_suffix(".cyd")
        meta_path = dest.with_suffix(dest.suffix + ".meta.json")
        meta = expected_meta(source, width, height, mode)
        if not force and dest.exists() and meta_matches(meta_path, meta):
            skipped += 1
            continue
        convert_image(source, dest, width, height, mode)
        meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        count += 1
        print(f"Converted {source.relative_to(content_dir).as_posix()} -> {dest.relative_to(content_dir).as_posix()} ({mode} {width}x{height})")
    print(f"Image conversion complete: converted {count}, skipped {skipped}")
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert JPG/PNG files into CYD RGB565 .cyd files.")
    parser.add_argument("content_dir", nargs="?", default=str(DEFAULT_CONTENT_DIR), help="Published content directory")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--mode", choices=("cover", "fit"), default="fit", help="fit letterboxes and preserves full image; cover crops to fill")
    parser.add_argument("--force", action="store_true", help="regenerate even if .cyd is newer than source")
    args = parser.parse_args()
    convert_all(Path(args.content_dir).resolve(), args.width, args.height, args.mode, args.force)


if __name__ == "__main__":
    main()
