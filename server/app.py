from __future__ import annotations

import configparser
import json
import os
import fnmatch
import re
import shutil
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from flask import Flask, Response, abort, redirect, request, send_file, url_for

ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = Path(os.environ.get("CYD_BANNERS_CONFIG", ROOT / "server" / "config.ini"))
SAMPLE_CONFIG_PATH = ROOT / "server" / "config.ini.sample"


def load_config() -> configparser.ConfigParser:
    config = configparser.ConfigParser()
    config.read(SAMPLE_CONFIG_PATH)
    if CONFIG_PATH.exists():
        config.read(CONFIG_PATH)
    return config


config = load_config()
HOST = config.get("server", "host", fallback="0.0.0.0")
PORT = config.getint("server", "port", fallback=8088)
BASE_PATH = config.get("server", "base_path", fallback="/cyd-banners").rstrip("/")


def token_from_lfs_config() -> str | None:
    data_config = ROOT / "data" / "config.txt"
    if not data_config.exists():
        return None
    for raw_line in data_config.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip().lower() == "service_token":
            value = value.strip()
            if value:
                return value
    return None


TOKEN = token_from_lfs_config() or ""
CONTENT_DIR = (ROOT / config.get("paths", "content_dir", fallback="server/content")).resolve()
STATE_DIR = (ROOT / config.get("paths", "state_dir", fallback="server/state")).resolve()
HEARTBEATS_PATH = STATE_DIR / "heartbeats.json"
CONVERSION_SETTINGS_PATH = STATE_DIR / "conversion_settings.json"
DEVICE_ALIASES_PATH = STATE_DIR / "device_aliases.json"
FIRMWARE_DIR = (ROOT / "server" / "firmware").resolve()
MANIFEST_PATH = CONTENT_DIR / "manifest.txt"
SUM_PATH = CONTENT_DIR / "sum.txt"
PLAYLIST_CHUNK_SIZE = config.getint("server", "playlist_chunk_size", fallback=100)
PLAYLIST_CACHE_DIR = STATE_DIR / "playlist_chunks"
SHUTDOWN_ENABLED = config.get("server", "shutdown_enabled", fallback="yes").lower() in ("yes", "true", "1")

app = Flask(__name__)
CONTENT_REFRESH_LOCK = threading.RLock()


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


MAC_RE = re.compile(r"^(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}$|^[0-9A-Fa-f]{12}$")
SAFE_MANIFEST_PATH_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9._/@+ -]*$")


def normalize_mac(mac: str) -> str | None:
    mac = (mac or "").strip()
    if not MAC_RE.fullmatch(mac):
        return None
    cleaned = re.sub(r"[^0-9A-Fa-f]", "", mac)
    return ":".join(cleaned[i : i + 2] for i in range(0, 12, 2)).upper()


def load_json(path: Path, default: Any) -> Any:
    try:
        if path.exists():
            return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        pass
    return default


def save_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


DEVICE_SIZES = [4, 8, 16, 32, 64, 128, 256, 512, 1024]


def load_device_aliases() -> dict[str, dict]:
    return load_json(DEVICE_ALIASES_PATH, {})


def save_device_aliases(aliases: dict[str, dict]) -> None:
    save_json(DEVICE_ALIASES_PATH, aliases)


def get_device_alias(mac: str) -> str:
    aliases = load_device_aliases()
    return aliases.get(mac, {}).get("alias", "") or mac


def get_device_sd_size_gb(mac: str) -> int:
    aliases = load_device_aliases()
    return aliases.get(mac, {}).get("sd_size_gb", 32)


def get_device_playlist(mac: str) -> str:
    return read_root_playlist_section(mac)


def read_root_playlist_section(mac: str) -> str:
    wanted = (mac or "").replace(":", "").replace("-", "").upper()
    if not wanted or not (CONTENT_DIR / "playlist.ini").is_file():
        return ""
    active = False
    lines: list[str] = []
    for raw in (CONTENT_DIR / "playlist.ini").read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().replace(":", "").replace("-", "").upper()
            active = section == wanted
            continue
        if active:
            lines.append(raw)
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines)


def write_root_playlist_section(mac: str, playlist_text: str) -> None:
    playlist_path = CONTENT_DIR / "playlist.ini"
    wanted = (mac or "").replace(":", "").replace("-", "").upper()
    if len(wanted) != 12:
        raise ValueError("invalid MAC")
    lines = playlist_path.read_text(encoding="utf-8").splitlines() if playlist_path.is_file() else []
    output: list[str] = []
    i = 0
    replaced = False
    section_header = f"[{mac}]"
    while i < len(lines):
        raw = lines[i]
        stripped = raw.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip().replace(":", "").replace("-", "").upper()
            if section == wanted:
                replaced = True
                output.append(section_header)
                for entry in playlist_text.splitlines():
                    output.append(entry.rstrip())
                i += 1
                while i < len(lines):
                    next_stripped = lines[i].strip()
                    if next_stripped.startswith("[") and next_stripped.endswith("]"):
                        break
                    i += 1
                continue
        output.append(raw)
        i += 1
    if not replaced:
        if output and output[-1].strip():
            output.append("")
        output.append(section_header)
        for entry in playlist_text.splitlines():
            output.append(entry.rstrip())
    playlist_path.write_text("\n".join(output).rstrip() + "\n", encoding="utf-8")


def update_device(mac: str, alias: str, sd_size_gb: int, playlist: str) -> None:
    aliases = load_device_aliases()
    if mac not in aliases:
        aliases[mac] = {}
    aliases[mac]["alias"] = alias
    aliases[mac]["sd_size_gb"] = sd_size_gb
    aliases[mac]["playlist"] = playlist
    save_device_aliases(aliases)


def get_device_sd_size_label(sd_size_gb: int) -> str:
    if sd_size_gb >= 1024:
        return f"{sd_size_gb // 1024} TB"
    return f"{sd_size_gb} GB"


def format_bytes(b: int) -> str:
    for unit, exp in [("PB", 50), ("TB", 40), ("GB", 30), ("MB", 20), ("KB", 10)]:
        if b >= 2 ** exp:
            return f"{b / (2 ** exp):.1f} {unit}"
    return f"{b} B"


def default_conversion_settings() -> dict[str, Any]:
    return {
        "auto_contrast": False,
        "contrast": 1.0,
        "brightness": 1.0,
        "saturation": 1.0,
        "gamma": 1.0,
        "dither": False,
    }


def infer_conversion_settings_from_meta() -> dict[str, Any]:
    for meta_path in sorted(CONTENT_DIR.rglob("*.cyd.meta.json")):
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        settings = default_conversion_settings()
        found = False
        for key in settings.keys():
            if key in meta:
                settings[key] = meta[key]
                found = True
        if found:
            return settings
    return default_conversion_settings()


def load_conversion_settings() -> dict[str, Any]:
    if CONVERSION_SETTINGS_PATH.exists():
        settings = default_conversion_settings()
        saved = load_json(CONVERSION_SETTINGS_PATH, {})
        if isinstance(saved, dict):
            settings.update({key: saved[key] for key in settings.keys() if key in saved})
        return settings
    settings = infer_conversion_settings_from_meta()
    save_conversion_settings(settings)
    return settings


def save_conversion_settings(settings: dict[str, Any]) -> None:
    save_json(CONVERSION_SETTINGS_PATH, settings)


def prepare_content(
    force_images: bool = False,
    auto_contrast: bool | None = None,
    contrast: float | None = None,
    brightness: float | None = None,
    saturation: float | None = None,
    gamma: float | None = None,
    dither: bool | None = None,
) -> tuple[int, str]:
    from tools_prepare_content import prepare_content as run_prepare

    settings = load_conversion_settings()
    return run_prepare(
        CONTENT_DIR,
        force_images=force_images,
        auto_contrast=settings["auto_contrast"] if auto_contrast is None else auto_contrast,
        contrast=settings["contrast"] if contrast is None else contrast,
        brightness=settings["brightness"] if brightness is None else brightness,
        saturation=settings["saturation"] if saturation is None else saturation,
        gamma=settings["gamma"] if gamma is None else gamma,
        dither=settings["dither"] if dither is None else dither,
    )


def require_sum_token() -> None:
    if not TOKEN or request.args.get("t") != TOKEN:
        abort(403)


def normalized_request_path(rel_path: str) -> str:
    rel_path = rel_path.replace("\\", "/").lstrip("/").strip()
    if not rel_path or "\x00" in rel_path or "//" in rel_path:
        abort(404)
    if any(part in ("", ".", "..") for part in rel_path.split("/")):
        abort(404)
    if not SAFE_MANIFEST_PATH_RE.fullmatch(rel_path):
        abort(404)
    return rel_path


def manifest_file_set() -> set[str]:
    if not MANIFEST_PATH.exists():
        prepare_content()
    allowed: set[str] = set()
    try:
        for raw_line in MANIFEST_PATH.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line.startswith("FILE "):
                continue
            parts = line.split(" ", 3)
            if len(parts) == 4:
                allowed.add(parts[3].strip().replace("\\", "/").lstrip("/"))
    except OSError:
        abort(404)
    return allowed


def content_rel(path: Path) -> str:
    return path.relative_to(CONTENT_DIR).as_posix()


def strip_duration(value: str) -> str:
    return value.rsplit("|", 1)[0].strip() if "|" in value else value.strip()


def resolve_playlist_value(value: str, base_dir: Path) -> tuple[str, Path | None, bool]:
    value = strip_duration(value)
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
        return value, (CONTENT_DIR / value).resolve(), True
    if value.startswith("/"):
        value = value.lstrip("/")
        if value.lower().startswith("banners/"):
            value = value[8:]
        return value, (CONTENT_DIR / value).resolve(), True
    resolved = (base_dir / value).resolve()
    try:
        return resolved.relative_to(CONTENT_DIR).as_posix(), resolved, True
    except ValueError:
        return value, resolved, True


def read_playlist_lines(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []


def split_playlist_duration(value: str) -> tuple[str, str]:
    if "|" not in value:
        return value.strip(), ""
    path, duration = value.rsplit("|", 1)
    return path.strip(), duration.strip()


def normalize_section_name(section: str) -> str:
    return re.sub(r"[:\-\s]", "", section.strip().upper())


def sd_display_path(rel_path: str) -> str:
    rel_path = rel_path.replace("\\", "/").lstrip("/")
    if rel_path.lower().startswith("banners/"):
        rel_path = rel_path[8:]
    return f"SD://banners/{rel_path}"


def render_flat_playlist_line(rel_or_lfs: str, duration: str) -> str:
    if rel_or_lfs.lower().startswith("lfs://"):
        path = rel_or_lfs
    else:
        path = sd_display_path(rel_or_lfs)
    return f"{path}|{duration}" if duration else path


def expand_playlist_file(path: Path, output: list[str], parsed: set[str]) -> None:
    try:
        rel_playlist = content_rel(path)
    except ValueError:
        return
    if rel_playlist in parsed:
        return
    parsed.add(rel_playlist)
    base_dir = path.parent
    for raw_line in read_playlist_lines(path):
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";") or (line.startswith("[") and line.endswith("]")):
            continue
        value, duration = split_playlist_duration(line)
        rel, resolved, is_sd = resolve_playlist_value(value, base_dir)
        lower_rel = rel.lower()
        if not is_sd:
            output.append(render_flat_playlist_line(rel, duration))
            continue
        if any(ch in rel for ch in "*?"):
            if resolved and resolved.is_absolute():
                try:
                    glob_pattern = resolved.relative_to(base_dir).as_posix()
                except ValueError:
                    glob_pattern = rel
            else:
                glob_pattern = rel
            for match in sorted((p for p in base_dir.glob(glob_pattern) if p.is_file()), key=lambda p: p.as_posix().lower()):
                if match.suffix.lower() in (".ini", ".play"):
                    expand_playlist_file(match, output, parsed)
                else:
                    output.append(render_flat_playlist_line(content_rel(match), duration))
            continue
        if lower_rel.endswith((".ini", ".play")) and resolved and resolved.is_file():
            expand_playlist_file(resolved, output, parsed)
        else:
            output.append(render_flat_playlist_line(rel, duration))


def expanded_playlist_for_mac(raw_mac: str) -> list[str]:
    mac = normalize_mac(raw_mac) or raw_mac
    wanted = normalize_section_name(mac)
    root_playlist = CONTENT_DIR / "playlist.ini"
    if not root_playlist.exists():
        return []
    parsed: set[str] = set()
    output: list[str] = []

    def process_root_pass(section_name: str, include_no_section: bool) -> bool:
        active = include_no_section
        matched = False
        base_dir = root_playlist.parent
        for raw_line in read_playlist_lines(root_playlist):
            line = raw_line.strip()
            if line.startswith("[") and line.endswith("]"):
                active = normalize_section_name(line[1:-1]) == section_name
                continue
            if not active or not line or line.startswith("#") or line.startswith(";"):
                continue
            matched = True
            value, duration = split_playlist_duration(line)
            rel, resolved, is_sd = resolve_playlist_value(value, base_dir)
            if not is_sd:
                output.append(render_flat_playlist_line(rel, duration))
            elif any(ch in rel for ch in "*?"):
                pattern = resolved.relative_to(base_dir).as_posix() if resolved and resolved.is_absolute() else rel
                for match in sorted((p for p in base_dir.glob(pattern) if p.is_file()), key=lambda p: p.as_posix().lower()):
                    if match.suffix.lower() in (".ini", ".play"):
                        expand_playlist_file(match, output, parsed)
                    else:
                        output.append(render_flat_playlist_line(content_rel(match), duration))
            elif resolved and resolved.is_file() and resolved.suffix.lower() in (".ini", ".play"):
                expand_playlist_file(resolved, output, parsed)
            else:
                output.append(render_flat_playlist_line(rel, duration))
        return matched

    process_root_pass("GLOBAL", True)
    if not process_root_pass(wanted, False):
        process_root_pass("DEFAULT", False)
    return output


def playlist_cache_mac_dir(mac: str) -> Path:
    return PLAYLIST_CACHE_DIR / normalize_section_name(mac)


def playlist_cache_file(mac: str, chunk_index: int) -> Path:
    return playlist_cache_mac_dir(mac) / f"playlist_{chunk_index:03d}.ini.{normalize_section_name(mac)}"


def generate_playlist_chunks_for_mac(mac: str) -> int:
    mac_dir = playlist_cache_mac_dir(mac)
    if mac_dir.exists():
        shutil.rmtree(mac_dir)
    mac_dir.mkdir(parents=True, exist_ok=True)
    lines = expanded_playlist_for_mac(mac)
    chunk_count = 0
    for start in range(0, len(lines), PLAYLIST_CHUNK_SIZE):
        chunk_path = playlist_cache_file(mac, chunk_count)
        chunk_text = "\n".join(lines[start:start + PLAYLIST_CHUNK_SIZE]) + "\n"
        chunk_path.write_text(chunk_text, encoding="utf-8")
        chunk_count += 1
    (mac_dir / "generated.json").write_text(
        json.dumps({"mac": mac, "chunks": chunk_count, "lines": len(lines), "chunk_size": PLAYLIST_CHUNK_SIZE, "generated_utc": utc_now()}, indent=2),
        encoding="utf-8",
    )
    return chunk_count


def playlist_chunk_for_mac(raw_mac: str, chunk_index: int) -> str:
    mac = normalize_mac(raw_mac)
    if not mac or chunk_index < 0:
        return ""
    if chunk_index == 0 or not playlist_cache_mac_dir(mac).exists():
        generate_playlist_chunks_for_mac(mac)
    chunk_path = playlist_cache_file(mac, chunk_index)
    if not chunk_path.exists():
        return ""
    return chunk_path.read_text(encoding="utf-8")


def analyze_content_status() -> dict[str, list[str]]:
    warnings: list[str] = []
    info: list[str] = []
    referenced_files: set[str] = set()
    referenced_dirs: set[str] = set()
    parsed_playlists: set[str] = set()

    def mark_ref(rel_path: str) -> None:
        rel_path = rel_path.replace("\\", "/").lstrip("/")
        referenced_files.add(rel_path)
        parts = rel_path.split("/")[:-1]
        while parts:
            referenced_dirs.add("/".join(parts))
            parts.pop()

    def parse_playlist(path: Path) -> None:
        try:
            rel_playlist = content_rel(path)
        except ValueError:
            return
        if rel_playlist in parsed_playlists:
            return
        parsed_playlists.add(rel_playlist)
        mark_ref(rel_playlist)
        base_dir = path.parent
        for raw_line in read_playlist_lines(path):
            line = raw_line.strip()
            if not line or line.startswith("#") or line.startswith(";") or (line.startswith("[") and line.endswith("]")):
                continue
            rel, resolved, is_sd = resolve_playlist_value(line, base_dir)
            if not is_sd:
                lfs_rel = rel[6:].lstrip("/") if rel.lower().startswith("lfs://") else rel
                lfs_path = (ROOT / "data" / lfs_rel).resolve()
                if not lfs_path.is_file():
                    warnings.append(f"LFS reference missing from project data directory: {rel} -> data/{lfs_rel}")
                continue
            if any(ch in rel for ch in "*?"):
                if resolved and resolved.is_absolute():
                    try:
                        glob_pattern = resolved.relative_to(base_dir).as_posix()
                    except ValueError:
                        glob_pattern = rel
                else:
                    glob_pattern = rel
                matches = sorted(p for p in base_dir.glob(glob_pattern) if p.is_file())
                if not matches:
                    warnings.append(f"Wildcard matched no files: {rel_playlist} -> {rel}")
                for match in matches:
                    mark_ref(content_rel(match))
                    if match.suffix.lower() in (".ini", ".play"):
                        parse_playlist(match)
                parent = resolved.parent if resolved else base_dir
                if parent.exists() and parent.is_dir() and parent.name.lower() == "images" and not any(parent.iterdir()):
                    warnings.append(f"Empty image directory referenced: {content_rel(parent)}")
                continue
            if resolved is None or not resolved.exists():
                warnings.append(f"Missing playlist reference: {rel_playlist} -> {rel}")
                mark_ref(rel)
                continue
            if resolved.is_file():
                mark_ref(content_rel(resolved))
                if resolved.suffix.lower() in (".ini", ".play"):
                    parse_playlist(resolved)
            elif resolved.is_dir():
                referenced_dirs.add(content_rel(resolved))

    root_playlist = CONTENT_DIR / "playlist.ini"
    if root_playlist.exists():
        parse_playlist(root_playlist)
    else:
        warnings.append("Missing root playlist.ini")

    for images_dir in sorted(CONTENT_DIR.rglob("images")):
        if images_dir.is_dir():
            cyd_files = list(images_dir.glob("*.cyd"))
            if not cyd_files:
                warnings.append(f"Empty image directory: {content_rel(images_dir)}")

    for gamelist in sorted(CONTENT_DIR.rglob("gamelist.txt")):
        base_dir = gamelist.parent
        gamelist_dirs: set[str] = set()
        for line_number, raw_line in enumerate(read_playlist_lines(gamelist), start=1):
            line = raw_line.strip()
            if not line or line.startswith("#") or line.startswith(";"):
                continue
            parts = line.split("|", 2)
            if len(parts) != 3:
                warnings.append(f"Invalid gamelist line: {content_rel(gamelist)}:{line_number}")
                continue
            rel_dir = parts[0].strip().replace("\\", "/").strip("/")
            if not rel_dir:
                warnings.append(f"Empty gamelist directory field: {content_rel(gamelist)}:{line_number}")
                continue
            gamelist_dirs.add(rel_dir.split("/", 1)[0])
            if not (base_dir / rel_dir).is_dir():
                warnings.append(f"Gamelist entry directory missing: {content_rel(gamelist)} -> {rel_dir}")
        ignored_gamelist_dirs = {"images", "__pycache__"}
        for child in sorted(p for p in base_dir.iterdir() if p.is_dir()):
            if child.name in ignored_gamelist_dirs:
                continue
            if child.name not in gamelist_dirs:
                warnings.append(f"Directory exists but is not listed in {content_rel(gamelist)}: {content_rel(child)}")

    ignored_dirs = {"__pycache__"}
    for directory in sorted(p for p in CONTENT_DIR.rglob("*") if p.is_dir()):
        rel = content_rel(directory)
        if directory.name in ignored_dirs:
            continue
        if rel not in referenced_dirs and not any(ref.startswith(rel + "/") for ref in referenced_files):
            info.append(f"Directory is not referenced by any playlist: {rel}")

    return {"warnings": sorted(set(warnings), key=str.lower), "info": sorted(set(info), key=str.lower), "playlists": sorted(parsed_playlists, key=str.lower)}


def safe_content_path(rel_path: str) -> Path:
    rel_path = normalized_request_path(rel_path)
    if rel_path not in manifest_file_set():
        abort(404)
    candidate = (CONTENT_DIR / rel_path).resolve()
    if not candidate.is_relative_to(CONTENT_DIR):
        abort(404)
    if not candidate.is_file():
        abort(404)
    return candidate


def record_heartbeat(raw_mac: str | None) -> None:
    mac = normalize_mac(raw_mac or "")
    if not mac:
        abort(400)
    heartbeats = load_json(HEARTBEATS_PATH, {})
    existing = heartbeats.get(mac, {})
    existing.setdefault("first_seen", utc_now())
    existing["last_seen"] = utc_now()
    existing["last_ip"] = request.headers.get("X-Forwarded-For", request.remote_addr or "").split(",")[0].strip()
    existing["call_count"] = int(existing.get("call_count", 0)) + 1
    if "update" in request.args:
        update_state = request.args.get("update", "").strip()
        existing["update"] = update_state[:120]
        for arg_name, field_name in (("p", "priority_count"), ("b", "background_count")):
            raw = request.args.get(arg_name, "").strip()
            if raw:
                try:
                    value = max(0, int(raw))
                except ValueError:
                    value = 0
            else:
                value = 0
            existing[field_name] = value
    heartbeats[mac] = existing
    save_json(HEARTBEATS_PATH, heartbeats)


def manifest_entries_from_text(text: str) -> dict[str, tuple[str, str]]:
    entries: dict[str, tuple[str, str]] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line.startswith("FILE "):
            continue
        parts = line.split(" ", 3)
        if len(parts) == 4:
            entries[parts[3]] = (parts[1], parts[2])
    return entries


def log_manifest_diff(before_text: str, after_text: str) -> None:
    if before_text == after_text:
        return
    before = manifest_entries_from_text(before_text)
    after = manifest_entries_from_text(after_text)
    added = sorted(set(after) - set(before), key=str.lower)
    removed = sorted(set(before) - set(after), key=str.lower)
    changed = sorted((path for path in set(before) & set(after) if before[path] != after[path]), key=str.lower)
    print(
        "Manifest changed: "
        f"added {len(added)}, removed {len(removed)}, changed {len(changed)}",
        flush=True,
    )
    for label, paths in (("ADDED", added), ("REMOVED", removed), ("CHANGED", changed)):
        for path in paths[:25]:
            print(f"  {label} {path}", flush=True)
        if len(paths) > 25:
            print(f"  {label} ... {len(paths) - 25} more", flush=True)


def refresh_indexes_without_image_conversion() -> str:
    from tools_generate_gamelists import generate_gamelists
    from tools_generate_playlist_chunks import generate_playlist_chunks
    from tools_generate_indexes import generate_indexes

    with CONTENT_REFRESH_LOCK:
        before_manifest = MANIFEST_PATH.read_text(encoding="utf-8") if MANIFEST_PATH.exists() else ""
        generate_gamelists(CONTENT_DIR)
        generate_playlist_chunks(CONTENT_DIR)
        _manifest_path, _sum_path, content_sum = generate_indexes(CONTENT_DIR)
        after_manifest = MANIFEST_PATH.read_text(encoding="utf-8") if MANIFEST_PATH.exists() else ""
        log_manifest_diff(before_manifest, after_manifest)
        return content_sum


@app.get(f"{BASE_PATH}/heartbeat")
def heartbeat() -> Response:
    require_sum_token()
    record_heartbeat(request.args.get("mac"))
    return Response("OK\n", mimetype="text/plain")


@app.get(f"{BASE_PATH}/sum.txt")
def sum_txt() -> Response:
    require_sum_token()
    record_heartbeat(request.args.get("mac"))
    refresh_indexes_without_image_conversion()
    return Response(SUM_PATH.read_text(encoding="utf-8"), mimetype="text/plain")


@app.get(f"{BASE_PATH}/manifest.txt")
def manifest_txt() -> Response:
    refresh_indexes_without_image_conversion()
    return Response(MANIFEST_PATH.read_text(encoding="utf-8"), mimetype="text/plain")


@app.get(f"{BASE_PATH}/firmware/latest.json")
def firmware_latest():
    latest_path = FIRMWARE_DIR / "latest.json"
    if not latest_path.is_file():
        abort(404)
    return send_file(latest_path, mimetype="application/json", as_attachment=False)


@app.get(f"{BASE_PATH}/firmware/firmware.bin")
def firmware_bin():
    firmware_path = FIRMWARE_DIR / "firmware.bin"
    if not firmware_path.is_file():
        abort(404)
    return send_file(firmware_path, mimetype="application/octet-stream", as_attachment=False)


@app.get(f"{BASE_PATH}/playlist_<int:chunk_index>.ini.<path:raw_mac>")
def playlist_chunk(chunk_index: int, raw_mac: str) -> Response:
    mac = normalize_mac(raw_mac)
    if not mac:
        abort(400)
    path = CONTENT_DIR / "_generated" / "playlists" / re.sub(r"[^0-9A-F]", "", mac.upper()) / f"playlist_{chunk_index:03d}.ini"
    if chunk_index == 0 and not path.exists():
        prepare_content()
    return Response(path.read_text(encoding="utf-8") if path.exists() else "", mimetype="text/plain")


@app.get(f"{BASE_PATH}/files/<path:rel_path>")
def content_file(rel_path: str):
    return send_file(safe_content_path(rel_path), as_attachment=False)


STATIC_DIR = (Path(__file__).resolve().parent / "static").resolve()


@app.get("/favicon.ico")
def favicon():
    path = ROOT / "favicon.ico"
    if not path.is_file():
        abort(404)
    return send_file(path, as_attachment=False)


@app.get(f"{BASE_PATH}/static/<path:rel_path>")
def static_file(rel_path: str):
    candidate = (STATIC_DIR / rel_path).resolve()
    if not candidate.is_relative_to(STATIC_DIR) or not candidate.is_file():
        abort(404)
    return send_file(candidate, as_attachment=False)


@app.get(f"{BASE_PATH}/api/directories")
def api_directories() -> Response:
    dirs = []
    for item in sorted(CONTENT_DIR.iterdir(), key=lambda p: p.name.lower()):
        if not item.is_dir() or item.name.startswith("_"):
            continue
        image_count = len(list(item.rglob("*.cyd")))
        total_size = sum(f.stat().st_size for f in item.rglob("*") if f.is_file())
        dirs.append({"name": item.name, "image_count": image_count, "total_size": total_size})
    return Response(json.dumps(dirs), mimetype="application/json")


@app.post(f"{BASE_PATH}/api/regenerate_dir")
def api_regenerate_dir() -> Response:
    dir_name = request.args.get("dir", "")
    if not dir_name:
        abort(400)
    dir_path = (CONTENT_DIR / dir_name).resolve()
    if not dir_path.is_dir() or not dir_path.is_relative_to(CONTENT_DIR):
        abort(404)
    from tools_convert_images import convert_all
    settings = load_conversion_settings()
    convert_all(
        dir_path,
        width=320, height=240, mode="fit",
        force=True,
        auto_contrast=bool(settings["auto_contrast"]),
        contrast=float(settings["contrast"]),
        brightness=float(settings["brightness"]),
        saturation=float(settings["saturation"]),
        gamma=float(settings["gamma"]),
        dither=bool(settings["dither"]),
        settings_sensitive=False,
    )
    return redirect(url_for("admin"))


@app.post(f"{BASE_PATH}/api/regenerate_device")
def api_regenerate_device() -> Response:
    mac = request.args.get("mac", "")
    if not normalize_mac(mac):
        abort(400)
    refresh_indexes_without_image_conversion()
    return redirect(url_for("admin"))


@app.get(f"{BASE_PATH}/api/devices")
def api_devices() -> Response:
    heartbeats = load_json(HEARTBEATS_PATH, {})
    aliases = load_device_aliases()
    devices = []
    for mac in sorted(heartbeats.keys()):
        info = heartbeats[mac]
        alias = aliases.get(mac, {}).get("alias", "")
        devices.append({
            "mac": mac,
            "alias": alias,
            "last_seen": info.get("last_seen", ""),
            "last_ip": info.get("last_ip", ""),
            "call_count": info.get("call_count", 0),
            "update": info.get("update", ""),
            "priority_count": int(info.get("priority_count", 0) or 0),
            "background_count": int(info.get("background_count", 0) or 0),
            "sd_size_gb": aliases.get(mac, {}).get("sd_size_gb", 32),
            "playlist": aliases.get(mac, {}).get("playlist", ""),
        })
    return Response(json.dumps(devices), mimetype="application/json")


@app.post(f"{BASE_PATH}/api/device")
def api_device_update() -> Response:
    mac = request.form.get("mac", "").strip()
    if not mac:
        abort(400)

    aliases = load_device_aliases()
    current = aliases.get(mac, {})
    alias = str(current.get("alias", ""))
    sd_size_gb = int(current.get("sd_size_gb", 32))
    playlist = str(current.get("playlist", ""))

    if "alias" in request.form:
        alias = request.form.get("alias", "").strip()
    playlist_changed = "playlist" in request.form
    if playlist_changed:
        playlist = request.form.get("playlist", "")
    if "sd_size_gb" in request.form:
        raw = request.form.get("sd_size_gb", "32").strip().upper()
        raw = raw.replace("GB", "")
        if raw.endswith("TB"):
            raw = str(int(float(raw[:-2]) * 1024))
        try:
            sd_size_gb = int(raw)
        except (ValueError, TypeError):
            return Response(json.dumps({"error": "invalid sd_size_gb"}), mimetype="application/json", status=400)

    if playlist_changed:
        write_root_playlist_section(mac, playlist)
    update_device(mac, alias, sd_size_gb, playlist)
    if playlist_changed:
        refresh_indexes_without_image_conversion()
    return Response(json.dumps({"ok": True}), mimetype="application/json")


@app.get(f"{BASE_PATH}/api/size")
def api_size() -> Response:
    total = sum(f.stat().st_size for f in CONTENT_DIR.rglob("*") if f.is_file())
    by_dir = {}
    for item in sorted(CONTENT_DIR.iterdir(), key=lambda p: p.name.lower()):
        if not item.is_dir():
            continue
        size = sum(f.stat().st_size for f in item.rglob("*") if f.is_file())
        by_dir[item.name] = size
    return Response(json.dumps({"total": total, "by_dir": by_dir}), mimetype="application/json")


@app.get(f"{BASE_PATH}/api/device_size")
def api_device_size() -> Response:
    mac = request.args.get("mac", "")
    if not mac:
        return Response(json.dumps({"error": "missing mac"}), mimetype="application/json")

    total_size = 0
    file_count = 0
    files = []
    if MANIFEST_PATH.is_file():
        for raw_line in MANIFEST_PATH.read_text(encoding="utf-8").splitlines():
            parts = raw_line.strip().split(" ", 3)
            if len(parts) == 4 and parts[0] == "FILE":
                try:
                    size = int(parts[2])
                except ValueError:
                    continue
                rel_path = parts[3]
                total_size += size
                file_count += 1
                if len(files) < 50:
                    files.append({"name": rel_path, "size": size})

    sd_size_gb = get_device_sd_size_gb(mac)
    sd_size_bytes = sd_size_gb * 1024 * 1024 * 1024
    return Response(json.dumps({
        "mac": mac,
        "content_size": total_size,
        "sd_size_gb": sd_size_gb,
        "sd_size_bytes": sd_size_bytes,
        "used_pct": round(total_size / sd_size_bytes * 100, 2) if sd_size_bytes > 0 else 0,
        "file_count": file_count,
        "files": files,
    }), mimetype="application/json")


@app.get(f"{BASE_PATH}/api/config")
def api_config() -> Response:
    cfg = {
        "host": HOST,
        "port": PORT,
        "base_path": BASE_PATH,
        "content_dir": str(CONTENT_DIR),
        "state_dir": str(STATE_DIR),
        "shutdown_enabled": SHUTDOWN_ENABLED,
        "token": TOKEN,
    }
    return Response(json.dumps(cfg), mimetype="application/json")


@app.post(f"{BASE_PATH}/api/config")
def api_config_update() -> Response:
    global HOST, PORT, BASE_PATH, SHUTDOWN_ENABLED, TOKEN
    HOST = request.form.get("host", HOST)
    PORT = int(request.form.get("port", PORT))
    BASE_PATH = request.form.get("base_path", BASE_PATH).rstrip("/")
    SHUTDOWN_ENABLED = request.form.get("shutdown_enabled", "0") == "1"
    TOKEN = request.form.get("token", TOKEN)
    return redirect(url_for("admin"))


@app.get("/admin/")
def admin() -> Response:
    from admin_page import render_admin_page

    sum_text = SUM_PATH.read_text(encoding="utf-8") if SUM_PATH.exists() else "sum.txt not generated yet"
    manifest_text = MANIFEST_PATH.read_text(encoding="utf-8") if MANIFEST_PATH.exists() else "manifest.txt not generated yet"
    return Response(
        render_admin_page(
            base_path=BASE_PATH,
            content_dir=CONTENT_DIR,
            host=HOST,
            port=PORT,
            token=TOKEN,
            shutdown_enabled=SHUTDOWN_ENABLED,
            sum_text=sum_text,
            manifest_text=manifest_text,
            conversion_settings=load_conversion_settings(),
            content_status=analyze_content_status(),
        ),
        mimetype="text/html",
    )

@app.get("/")
def root() -> Response:
    return Response('<a href="/admin/">CYD Banners admin</a>', mimetype="text/html")


@app.post("/admin/reset-heartbeats")
def admin_reset_heartbeats() -> Response:
    try:
        HEARTBEATS_PATH.unlink()
    except FileNotFoundError:
        pass
    return redirect(url_for("admin"))


@app.post("/admin/shutdown")
def admin_shutdown() -> Response:
    warning = (
        "\n\033[91m\033[1m"
        "============================================================\n"
        "  CYD BANNERS SERVER SHUTDOWN REQUESTED\n"
        "  SERVER IS STOPPING / STOPPED\n"
        "  PORT 8088 WILL NOT RESPOND UNTIL RESTARTED\n"
        "  RESTART SERVER MANUALLY WHEN READY\n"
        "============================================================\n"
        "\033[0m"
    )
    print(warning, flush=True)
    threading.Timer(0.25, lambda: os._exit(0)).start()
    return Response("CYD Banners server shutting down\n", mimetype="text/plain")


def form_float(name: str, default: float = 1.0) -> float:
    try:
        return float(request.form.get(name, str(default)))
    except ValueError:
        return default


@app.post("/admin/regenerate")
def admin_regenerate():
    force_images = request.form.get("force_images") == "1"
    auto_contrast = request.form.get("auto_contrast") == "1"
    dither = request.form.get("dither") == "1"
    brightness = form_float("brightness")
    contrast = form_float("contrast")
    saturation = form_float("saturation")
    gamma = form_float("gamma")
    settings = {
        "auto_contrast": auto_contrast,
        "contrast": contrast,
        "brightness": brightness,
        "saturation": saturation,
        "gamma": gamma,
        "dither": dither,
    }
    save_conversion_settings(settings)
    before_manifest = MANIFEST_PATH.read_text(encoding="utf-8") if MANIFEST_PATH.exists() else ""
    converted, content_sum = prepare_content(force_images=force_images, **settings)
    after_manifest = MANIFEST_PATH.read_text(encoding="utf-8") if MANIFEST_PATH.exists() else ""
    log_manifest_diff(before_manifest, after_manifest)
    print(
        "Admin regenerated content: "
        f"converted images {converted}, sum {content_sum}, "
        f"force_images={force_images}, auto_contrast={auto_contrast}, dither={dither}, "
        f"brightness={brightness:g}, contrast={contrast:g}, saturation={saturation:g}, gamma={gamma:g}"
    )
    return redirect(url_for("admin"))


if __name__ == "__main__":
    print(f"CYD Banners server: http://{HOST}:{PORT}/")
    print(f"Update base path: {BASE_PATH}")
    app.run(host=HOST, port=PORT)
