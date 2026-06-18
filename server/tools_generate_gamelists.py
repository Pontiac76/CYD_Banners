from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTENT_DIR = ROOT / "server" / "content"

SAFE_PATH_PART_RE = re.compile(r"^[A-Za-z0-9._ -]+$")
SAFE_SHORTHAND_RE = re.compile(r"^[A-Za-z0-9._-]+$")


@dataclass(frozen=True)
class GameEntry:
    line_number: int
    rel_path: str
    shorthand: str
    title_text: str


def is_safe_rel_path(value: str) -> bool:
    if not value or value.startswith(('/', '\\')) or '\x00' in value:
        return False
    parts = value.replace('\\', '/').split('/')
    return all(part not in ('', '.', '..') and SAFE_PATH_PART_RE.fullmatch(part) for part in parts)


def parse_title_text(value: str) -> str:
    return value.replace(r"\n", "\n")


def parse_gamelist(path: Path) -> list[GameEntry]:
    entries: list[GameEntry] = []
    seen_paths: dict[str, int] = {}
    seen_shorthands: dict[str, int] = {}
    errors: list[str] = []

    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith('#') or line.startswith(';'):
            continue
        parts = line.split('|', 2)
        if len(parts) != 3:
            errors.append(f"{path}:{line_number}: expected path|shorthand|name")
            continue
        rel_path, shorthand, title_text = (part.strip() for part in parts)
        if not is_safe_rel_path(rel_path):
            errors.append(f"{path}:{line_number}: unsafe relative path {rel_path!r}")
        if not SAFE_SHORTHAND_RE.fullmatch(shorthand):
            errors.append(f"{path}:{line_number}: unsafe shorthand {shorthand!r}")
        if not title_text:
            errors.append(f"{path}:{line_number}: empty name/title field")
        if rel_path in seen_paths:
            errors.append(f"{path}:{line_number}: duplicate path {rel_path!r}; first seen on line {seen_paths[rel_path]}")
        if shorthand in seen_shorthands:
            errors.append(f"{path}:{line_number}: duplicate shorthand {shorthand!r}; first seen on line {seen_shorthands[shorthand]}")
        seen_paths[rel_path] = line_number
        seen_shorthands[shorthand] = line_number
        entries.append(GameEntry(line_number, rel_path, shorthand, parse_title_text(title_text)))

    if errors:
        raise ValueError("\n".join(errors))
    return entries


def uncommented_playlist_entry(line: str) -> str:
    stripped = line.strip()
    if not stripped:
        return ""
    if stripped[0] in ("#", ";"):
        stripped = stripped[1:].strip()
    return stripped


def generate_from_gamelist(gamelist_path: Path) -> int:
    base_dir = gamelist_path.parent
    entries = parse_gamelist(gamelist_path)
    desired_parent_entries: list[str] = []
    changed = 0

    for entry in entries:
        game_dir = base_dir / Path(entry.rel_path)
        images_dir = game_dir / "images"
        game_dir.mkdir(parents=True, exist_ok=True)
        images_dir.mkdir(parents=True, exist_ok=True)

        about_path = game_dir / f"{entry.shorthand}_about.txt"
        if not about_path.exists():
            about_path.write_text(entry.title_text.rstrip("\n") + "\n", encoding="utf-8")
            changed += 1

        playlist_path = game_dir / f"{entry.shorthand}_playlist.ini"
        playlist_text = f"{entry.shorthand}_about.txt|3\nimages/{entry.shorthand}_*.cyd|3\n"
        if not playlist_path.exists():
            playlist_path.write_text(playlist_text, encoding="utf-8")
            changed += 1

        desired_parent_entries.append(f"{entry.rel_path}/{entry.shorthand}_playlist.ini")

    parent_playlist_path = base_dir / "playlist.ini"
    if parent_playlist_path.exists():
        existing_text = parent_playlist_path.read_text(encoding="utf-8")
        existing_lines = existing_text.splitlines()
        existing_entries = {uncommented_playlist_entry(line) for line in existing_lines}
        missing_entries = [entry for entry in desired_parent_entries if entry not in existing_entries]
        if missing_entries:
            parent_playlist_text = existing_text
            if parent_playlist_text and not parent_playlist_text.endswith("\n"):
                parent_playlist_text += "\n"
            parent_playlist_text += "\n".join(missing_entries) + "\n"
            parent_playlist_path.write_text(parent_playlist_text, encoding="utf-8")
            changed += 1
    elif desired_parent_entries:
        parent_playlist_text = "\n".join(desired_parent_entries) + "\n"
        parent_playlist_path.write_text(parent_playlist_text, encoding="utf-8")
        changed += 1

    return changed


def generate_gamelists(content_dir: Path = DEFAULT_CONTENT_DIR) -> int:
    content_dir = content_dir.resolve()
    changed = 0
    for gamelist_path in sorted(content_dir.rglob("gamelist.txt")):
        changed += generate_from_gamelist(gamelist_path)
    return changed


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate directory-local game content skeletons from gamelist.txt files.")
    parser.add_argument("content_dir", nargs="?", default=str(DEFAULT_CONTENT_DIR), help="Published content directory")
    args = parser.parse_args()
    changed = generate_gamelists(Path(args.content_dir))
    print(f"Generated gamelist content: changed files {changed}")


if __name__ == "__main__":
    main()
