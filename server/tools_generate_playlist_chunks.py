from __future__ import annotations

import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTENT_DIR = ROOT / "server" / "content"
GENERATED_DIR_NAME = "_generated"
DEFAULT_CHUNK_SIZE = 25

MAC_SECTION_RE = re.compile(r"^(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}$|^[0-9A-Fa-f]{12}$")


def normalize_mac(mac: str) -> str:
    cleaned = re.sub(r"[^0-9A-Fa-f]", "", mac)
    if len(cleaned) != 12:
        return ""
    return cleaned.upper()


def normalize_section_name(section: str) -> str:
    return re.sub(r"[:\-\s]", "", section.strip().upper())


def read_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []



def split_duration(value: str) -> tuple[str, str]:
    if "|" not in value:
        return value.strip(), ""
    path, duration = value.rsplit("|", 1)
    return path.strip(), duration.strip()


def content_rel(content_dir: Path, path: Path) -> str:
    return path.relative_to(content_dir).as_posix()


def sd_display_path(rel_path: str) -> str:
    rel_path = rel_path.replace("\\", "/").lstrip("/")
    if rel_path.lower().startswith("banners/"):
        rel_path = rel_path[8:]
    return f"SD://banners/{rel_path}"


def render_line(rel_or_lfs: str, duration: str) -> str:
    path = rel_or_lfs if rel_or_lfs.lower().startswith("lfs://") else sd_display_path(rel_or_lfs)
    return f"{path}|{duration}" if duration else path


def resolve_value(content_dir: Path, value: str, base_dir: Path) -> tuple[str, Path | None, bool]:
    value = split_duration(value)[0]
    if ":" in value:
        key, rest = value.split(":", 1)
        if key.strip().upper() == "PLAYLIST":
            value = rest.strip()
    lower = value.lower()
    if lower.startswith("lfs://"):
        return value, None, False
    if lower.startswith("sd://"):
        value = value[5:].lstrip("/")
        if value.lower().startswith("banners/"):
            value = value[8:]
        return value, (content_dir / value).resolve(), True
    if value.startswith("/"):
        value = value.lstrip("/")
        if value.lower().startswith("banners/"):
            value = value[8:]
        return value, (content_dir / value).resolve(), True
    resolved = (base_dir / value).resolve()
    try:
        return resolved.relative_to(content_dir).as_posix(), resolved, True
    except ValueError:
        return value, resolved, True


def expand_playlist_file(content_dir: Path, path: Path, output: list[str], parsed: set[str]) -> None:
    try:
        rel_playlist = content_rel(content_dir, path)
    except ValueError:
        return
    if rel_playlist in parsed:
        return
    parsed.add(rel_playlist)
    base_dir = path.parent
    for raw in read_lines(path):
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith(";") or (line.startswith("[") and line.endswith("]")):
            continue
        value, duration = split_duration(line)
        rel, resolved, is_sd = resolve_value(content_dir, value, base_dir)
        if not is_sd:
            output.append(render_line(rel, duration))
            continue
        if any(ch in rel for ch in "*?"):
            if resolved and resolved.is_absolute():
                try:
                    pattern = resolved.relative_to(base_dir).as_posix()
                except ValueError:
                    pattern = rel
            else:
                pattern = rel
            for match in sorted((p for p in base_dir.glob(pattern) if p.is_file()), key=lambda p: p.as_posix().lower()):
                if match.suffix.lower() in (".ini", ".play"):
                    expand_playlist_file(content_dir, match, output, parsed)
                else:
                    output.append(render_line(content_rel(content_dir, match), duration))
            continue
        if resolved and resolved.is_file() and resolved.suffix.lower() in (".ini", ".play"):
            expand_playlist_file(content_dir, resolved, output, parsed)
        else:
            output.append(render_line(rel, duration))


def mac_sections(content_dir: Path) -> list[str]:
    root_playlist = content_dir / "playlist.ini"
    macs: list[str] = []
    for raw in read_lines(root_playlist):
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            if MAC_SECTION_RE.fullmatch(section):
                mac = normalize_mac(section)
                if mac and mac not in macs:
                    macs.append(mac)
    return macs


def expanded_playlist_for_mac(content_dir: Path, mac: str) -> list[str]:
    root_playlist = content_dir / "playlist.ini"
    wanted = normalize_section_name(mac)
    parsed: set[str] = set()
    output: list[str] = []

    def process_entry(line: str, base_dir: Path) -> None:
        value, duration = split_duration(line)
        rel, resolved, is_sd = resolve_value(content_dir, value, base_dir)
        if not is_sd:
            output.append(render_line(rel, duration))
        elif any(ch in rel for ch in "*?"):
            pattern = resolved.relative_to(base_dir).as_posix() if resolved and resolved.is_absolute() else rel
            for match in sorted((p for p in base_dir.glob(pattern) if p.is_file()), key=lambda p: p.as_posix().lower()):
                if match.suffix.lower() in (".ini", ".play"):
                    expand_playlist_file(content_dir, match, output, parsed)
                else:
                    output.append(render_line(content_rel(content_dir, match), duration))
        elif resolved and resolved.is_file() and resolved.suffix.lower() in (".ini", ".play"):
            expand_playlist_file(content_dir, resolved, output, parsed)
        else:
            output.append(render_line(rel, duration))

    def process_root_pass(section_name: str, include_no_section: bool) -> bool:
        active = include_no_section
        matched = False
        base_dir = root_playlist.parent
        for raw in read_lines(root_playlist):
            line = raw.strip()
            if line.startswith("[") and line.endswith("]"):
                active = normalize_section_name(line[1:-1]) == section_name
                continue
            if not active or not line or line.startswith("#") or line.startswith(";"):
                continue
            matched = True
            process_entry(line, base_dir)
        return matched

    process_root_pass("GLOBAL", True)
    if not process_root_pass(wanted, False):
        process_root_pass("DEFAULT", False)
    return output


def generate_playlist_chunks(content_dir: Path = DEFAULT_CONTENT_DIR, chunk_size: int = DEFAULT_CHUNK_SIZE) -> int:
    content_dir = content_dir.resolve()
    generated_root = content_dir / GENERATED_DIR_NAME / "playlists"
    active_macs = mac_sections(content_dir)
    if generated_root.exists():
        for child in generated_root.iterdir():
            if child.is_dir() and child.name not in active_macs:
                shutil.rmtree(child)
    generated_root.mkdir(parents=True, exist_ok=True)

    changed = 0
    for mac in active_macs:
        mac_dir = generated_root / mac
        if mac_dir.exists():
            shutil.rmtree(mac_dir)
        mac_dir.mkdir(parents=True, exist_ok=True)
        lines = expanded_playlist_for_mac(content_dir, mac)
        for chunk_index, start in enumerate(range(0, len(lines), chunk_size)):
            text = "\n".join(lines[start:start + chunk_size]) + "\n"
            (mac_dir / f"playlist_{chunk_index:03d}.ini").write_text(text, encoding="utf-8")
            changed += 1
    return changed


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Generate expanded per-MAC playlist chunks under server/content/_generated/playlists.")
    parser.add_argument("content_dir", nargs="?", default=str(DEFAULT_CONTENT_DIR))
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE)
    args = parser.parse_args()
    count = generate_playlist_chunks(Path(args.content_dir), args.chunk_size)
    print(f"Generated playlist chunks: files {count}")
