from __future__ import annotations

import configparser
import json
import os
import fnmatch
import re
from html import escape
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
MANIFEST_PATH = CONTENT_DIR / "manifest.txt"
SUM_PATH = CONTENT_DIR / "sum.txt"

app = Flask(__name__)


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


MAC_RE = re.compile(r"^(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}$|^[0-9A-Fa-f]{12}$")
SAFE_MANIFEST_PATH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/@+ -]*$")


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


def prepare_content(
    force_images: bool = False,
    auto_contrast: bool = False,
    contrast: float = 1.0,
    brightness: float = 1.0,
    saturation: float = 1.0,
    gamma: float = 1.0,
    dither: bool = False,
) -> tuple[int, str]:
    from tools_prepare_content import prepare_content as run_prepare

    return run_prepare(
        CONTENT_DIR,
        force_images=force_images,
        auto_contrast=auto_contrast,
        contrast=contrast,
        brightness=brightness,
        saturation=saturation,
        gamma=gamma,
        dither=dither,
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


def render_status_items(items: list[str], empty: str) -> str:
    if not items:
        return f"<li>{escape(empty)}</li>"
    return "".join(f"<li>{escape(item)}</li>" for item in items)


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
    heartbeats[mac] = existing
    save_json(HEARTBEATS_PATH, heartbeats)


@app.get(f"{BASE_PATH}/sum.txt")
def sum_txt() -> Response:
    require_sum_token()
    record_heartbeat(request.args.get("mac"))
    if not SUM_PATH.exists():
        prepare_content()
    return Response(SUM_PATH.read_text(encoding="utf-8"), mimetype="text/plain")


@app.get(f"{BASE_PATH}/manifest.txt")
def manifest_txt() -> Response:
    if not MANIFEST_PATH.exists():
        prepare_content()
    return Response(MANIFEST_PATH.read_text(encoding="utf-8"), mimetype="text/plain")


@app.get(f"{BASE_PATH}/files/<path:rel_path>")
def content_file(rel_path: str):
    return send_file(safe_content_path(rel_path), as_attachment=False)


@app.get("/admin/")
def admin() -> Response:
    heartbeats = load_json(HEARTBEATS_PATH, {})
    rows = []
    for mac, info in sorted(heartbeats.items(), key=lambda item: item[1].get("last_seen", ""), reverse=True):
        rows.append(
            "<tr>"
            f"<td>{escape(mac)}</td>"
            f"<td>{escape(str(info.get('last_seen', '')))}</td>"
            f"<td>{escape(str(info.get('last_ip', '')))}</td>"
            f"<td>{escape(str(info.get('call_count', 0)))}</td>"
            "</tr>"
        )
    body = "\n".join(rows) or '<tr><td colspan="4">No CYDs have called home yet.</td></tr>'
    sum_text = SUM_PATH.read_text(encoding="utf-8") if SUM_PATH.exists() else "sum.txt not generated yet"
    manifest_text = MANIFEST_PATH.read_text(encoding="utf-8") if MANIFEST_PATH.exists() else "manifest.txt not generated yet"
    content_status = analyze_content_status()
    warning_items = render_status_items(content_status["warnings"], "No content warnings found.")
    info_items = render_status_items(content_status["info"], "No orphan/info items found.")
    playlist_items = render_status_items(content_status["playlists"], "No playlists parsed.")
    html = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>CYD Banners</title>
<style>
body{{font-family:Segoe UI,Arial,sans-serif;margin:2rem;background:#fff;color:#111}}
a{{color:#0645ad}}code,pre{{background:#f2f2f2;padding:.1rem .25rem;border-radius:.2rem}}
pre{{padding:1rem;overflow:auto;max-height:22rem;white-space:pre-wrap}}
table{{border-collapse:collapse}}td,th{{border:1px solid #ccc;padding:.35rem .6rem}}th{{background:#eee}}
.panel{{border:1px solid #ccc;border-radius:.35rem;padding:1rem;margin:1rem 0}}
.warn{{color:#b00020}}.info{{color:#666}}
@media (prefers-color-scheme: dark){{
  body{{background:#111;color:#ddd}}
  a{{color:#8ab4f8}}
  code,pre{{background:#222;color:#eee}}
  td,th{{border-color:#444}}
  th{{background:#222}}
  .panel{{border-color:#444}}
  .warn{{color:#ff8080}}.info{{color:#aaa}}
}}
</style>
</head><body>
<h1>CYD Banners Server</h1>
<p>Base path: <code>{escape(BASE_PATH)}</code> &nbsp; Content: <code>{escape(str(CONTENT_DIR))}</code><br>
Last sum: <code>{escape(sum_text.strip())}</code></p>
<form id="regen-form" method="post" action="/admin/regenerate" style="margin:1rem 0">
  <fieldset style="display:inline-block;padding:1rem">
    <legend>Regenerate content files</legend>
    <label><input name="force_images" type="checkbox" value="1"> Force image regeneration</label><br>
    <label><input name="auto_contrast" type="checkbox" value="1"> Auto contrast</label><br>
    <label><input name="dither" type="checkbox" value="1"> Dither RGB565</label><br><br>
    <label style="display:block;margin:.15rem 0">Brightness <input name="brightness" type="number" step="0.05" value="1.0" style="width:5rem"></label>
    <label style="display:block;margin:.15rem 0">Contrast <input name="contrast" type="number" step="0.05" value="1.0" style="width:5rem"></label>
    <label style="display:block;margin:.15rem 0">Saturation <input name="saturation" type="number" step="0.05" value="1.0" style="width:5rem"></label>
    <label style="display:block;margin:.15rem 0">Gamma <input name="gamma" type="number" step="0.05" value="1.0" style="width:5rem"></label>
    <button type="submit" style="margin-top:.4rem">Regenerate</button>
  </fieldset>
</form>
<div class="panel">
  <h2>Content status</h2>
  <h3 class="warn">Warnings ({len(content_status["warnings"])})</h3>
  <ul>{warning_items}</ul>
  <details><summary>Info / unreferenced directories ({len(content_status["info"])})</summary><ul class="info">{info_items}</ul></details>
  <details><summary>Parsed playlists ({len(content_status["playlists"])})</summary><ul>{playlist_items}</ul></details>
</div>
<table><thead><tr><th>MAC</th><th>Last Seen UTC</th><th>IP</th><th>Calls</th></tr></thead><tbody>{body}</tbody></table>
<details><summary><strong>manifest.txt</strong></summary>
<pre>{escape(manifest_text)}</pre>
</details>
<script>
const cookieName = 'cydBannersAdminImageSettings';
const form = document.getElementById('regen-form');
function loadSettings() {{
  const match = document.cookie.split('; ').find(row => row.startsWith(cookieName + '='));
  if (!match) return;
  try {{
    const settings = JSON.parse(decodeURIComponent(match.split('=')[1]));
    for (const [key, value] of Object.entries(settings)) {{
      const field = form.elements[key];
      if (!field) continue;
      if (field.type === 'checkbox') field.checked = !!value;
      else field.value = value;
    }}
  }} catch (_) {{}}
}}
function saveSettings() {{
  const settings = {{}};
  for (const field of form.elements) {{
    if (!field.name) continue;
    settings[field.name] = field.type === 'checkbox' ? field.checked : field.value;
  }}
  document.cookie = cookieName + '=' + encodeURIComponent(JSON.stringify(settings)) + '; max-age=31536000; path=/admin/; samesite=lax';
}}
loadSettings();
form.addEventListener('change', saveSettings);
form.addEventListener('submit', saveSettings);
</script>
</body></html>"""
    return Response(html, mimetype="text/html")


@app.get("/")
def root() -> Response:
    return Response('<a href="/admin/">CYD Banners admin</a>', mimetype="text/html")


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
    converted, content_sum = prepare_content(
        force_images=force_images,
        auto_contrast=auto_contrast,
        contrast=contrast,
        brightness=brightness,
        saturation=saturation,
        gamma=gamma,
        dither=dither,
    )
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
