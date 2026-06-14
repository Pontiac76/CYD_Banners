from __future__ import annotations

import argparse
from pathlib import Path

from tools_convert_images import convert_all
from tools_generate_indexes import generate_indexes

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTENT_DIR = ROOT / "server" / "content"


def prepare_content(content_dir: Path = DEFAULT_CONTENT_DIR, force_images: bool = False) -> tuple[int, str]:
    content_dir = content_dir.resolve()
    converted = convert_all(content_dir, width=320, height=240, mode="fit", force=force_images)
    _manifest_path, _sum_path, content_sum = generate_indexes(content_dir)
    return converted, content_sum


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert images and regenerate CYD manifest/sum files.")
    parser.add_argument("content_dir", nargs="?", default=str(DEFAULT_CONTENT_DIR), help="Published content directory")
    parser.add_argument("--force-images", action="store_true", help="force reconversion of all source images")
    args = parser.parse_args()
    converted, content_sum = prepare_content(Path(args.content_dir), force_images=args.force_images)
    print(f"Prepared content: converted images {converted}, sum {content_sum}")


if __name__ == "__main__":
    main()
