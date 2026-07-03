from __future__ import annotations

import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTENT_DIR = ROOT / "server" / "content"
SKIP_NAMES = {"manifest.txt", "sum.txt"}
SKIP_EXTENSIONS = {".jpg", ".jpeg", ".png"}
SKIP_SUFFIXES = {".cyd.meta.json"}


def file_hash(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 64), b""):
            digest.update(chunk)
    return digest.hexdigest()


def iter_content_files(content_dir: Path) -> list[Path]:
    files: list[Path] = []
    for path in content_dir.rglob("*"):
        if not path.is_file() or path.name in SKIP_NAMES or path.suffix.lower() in SKIP_EXTENSIONS:
            continue
        if any(path.name.lower().endswith(suffix) for suffix in SKIP_SUFFIXES):
            continue
        files.append(path)
    return sorted(files, key=lambda p: p.relative_to(content_dir).as_posix().lower())


def generate_indexes(content_dir: Path = DEFAULT_CONTENT_DIR) -> tuple[Path, Path, str]:
    content_dir = content_dir.resolve()
    content_dir.mkdir(parents=True, exist_ok=True)

    lines = ["ROOT playlist.ini", "HASH md5"]
    for path in iter_content_files(content_dir):
        rel = path.relative_to(content_dir).as_posix()
        try:
            size = path.stat().st_size
            digest = file_hash(path)
        except FileNotFoundError:
            # A generated file was replaced while this manifest walk was in progress.
            # The caller normally serializes content refreshes, but tolerate external
            # changes instead of returning HTTP 500 to devices.
            continue
        lines.append(f"FILE {digest} {size} {rel}")

    manifest = "\n".join(lines) + "\n"
    manifest_path = content_dir / "manifest.txt"
    sum_path = content_dir / "sum.txt"
    manifest_path.write_text(manifest, encoding="utf-8")
    content_sum = hashlib.md5(manifest.encode("utf-8")).hexdigest()
    sum_path.write_text(content_sum + "\n", encoding="utf-8")
    return manifest_path, sum_path, content_sum


if __name__ == "__main__":
    content_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CONTENT_DIR
    manifest_path, sum_path, content_sum = generate_indexes(content_dir)
    print(f"Manifest: {manifest_path}")
    print(f"Sum:      {sum_path}")
    print(f"MD5:      {content_sum}")
