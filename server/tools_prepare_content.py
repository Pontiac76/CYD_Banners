from __future__ import annotations

import argparse
from pathlib import Path

from tools_convert_images import convert_all
from tools_generate_gamelists import generate_gamelists
from tools_generate_indexes import generate_indexes
from tools_generate_playlist_chunks import generate_playlist_chunks

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTENT_DIR = ROOT / "server" / "content"


def prepare_content(
    content_dir: Path = DEFAULT_CONTENT_DIR,
    force_images: bool = False,
    auto_contrast: bool = False,
    contrast: float = 1.0,
    brightness: float = 1.0,
    saturation: float = 1.0,
    gamma: float = 1.0,
    dither: bool = False,
) -> tuple[int, str]:
    content_dir = content_dir.resolve()
    generate_gamelists(content_dir)
    converted = convert_all(
        content_dir,
        width=320,
        height=240,
        mode="fit",
        force=force_images,
        auto_contrast=auto_contrast,
        contrast=contrast,
        brightness=brightness,
        saturation=saturation,
        gamma=gamma,
        dither=dither,
        settings_sensitive=force_images,
    )
    generate_playlist_chunks(content_dir)
    _manifest_path, _sum_path, content_sum = generate_indexes(content_dir)
    return converted, content_sum


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert images and regenerate CYD manifest/sum files.")
    parser.add_argument("content_dir", nargs="?", default=str(DEFAULT_CONTENT_DIR), help="Published content directory")
    parser.add_argument("--force-images", action="store_true", help="force reconversion of all source images")
    parser.add_argument("--auto-contrast", action="store_true", help="stretch each image's color range before RGB565 conversion")
    parser.add_argument("--contrast", type=float, default=1.0, help="contrast multiplier")
    parser.add_argument("--brightness", type=float, default=1.0, help="brightness multiplier")
    parser.add_argument("--saturation", type=float, default=1.0, help="color saturation multiplier")
    parser.add_argument("--gamma", type=float, default=1.0, help="gamma correction")
    parser.add_argument("--dither", action="store_true", help="apply Floyd-Steinberg dithering when reducing to RGB565")
    args = parser.parse_args()
    converted, content_sum = prepare_content(
        Path(args.content_dir),
        force_images=args.force_images,
        auto_contrast=args.auto_contrast,
        contrast=args.contrast,
        brightness=args.brightness,
        saturation=args.saturation,
        gamma=args.gamma,
        dither=args.dither,
    )
    print(f"Prepared content: converted images {converted}, sum {content_sum}")


if __name__ == "__main__":
    main()
