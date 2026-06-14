from __future__ import annotations

import configparser
import json
import os
import re
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


def normalize_mac(mac: str) -> str | None:
    cleaned = re.sub(r"[^0-9A-Fa-f]", "", mac or "")
    if len(cleaned) != 12:
        return None
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


def prepare_content(force_images: bool = False) -> tuple[int, str]:
    from tools_prepare_content import prepare_content as run_prepare

    return run_prepare(CONTENT_DIR, force_images=force_images)


def require_sum_token() -> None:
    if not TOKEN or request.args.get("t") != TOKEN:
        abort(403)


def safe_content_path(rel_path: str) -> Path:
    rel_path = rel_path.replace("\\", "/").lstrip("/")
    candidate = (CONTENT_DIR / rel_path).resolve()
    if not candidate.is_relative_to(CONTENT_DIR):
        abort(404)
    if not candidate.is_file():
        abort(404)
    return candidate


def record_heartbeat(raw_mac: str | None) -> None:
    mac = normalize_mac(raw_mac or "")
    if not mac:
        return
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
            f"<td>{mac}</td>"
            f"<td>{info.get('last_seen', '')}</td>"
            f"<td>{info.get('last_ip', '')}</td>"
            f"<td>{info.get('call_count', 0)}</td>"
            "</tr>"
        )
    body = "\n".join(rows) or '<tr><td colspan="4">No CYDs have called home yet.</td></tr>'
    sum_text = SUM_PATH.read_text(encoding="utf-8") if SUM_PATH.exists() else "sum.txt not generated yet"
    manifest_text = MANIFEST_PATH.read_text(encoding="utf-8") if MANIFEST_PATH.exists() else "manifest.txt not generated yet"
    html = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>CYD Banners</title>
<style>
body{{font-family:Segoe UI,Arial,sans-serif;margin:2rem;background:#fff;color:#111}}
a{{color:#0645ad}}code,pre{{background:#f2f2f2;padding:.1rem .25rem;border-radius:.2rem}}
pre{{padding:1rem;overflow:auto;max-height:22rem;white-space:pre-wrap}}
table{{border-collapse:collapse}}td,th{{border:1px solid #ccc;padding:.35rem .6rem}}th{{background:#eee}}
@media (prefers-color-scheme: dark){{
  body{{background:#111;color:#ddd}}
  a{{color:#8ab4f8}}
  code,pre{{background:#222;color:#eee}}
  td,th{{border-color:#444}}
  th{{background:#222}}
}}
</style>
</head><body>
<h1>CYD Banners Server</h1>
<p>Base path: <code>{BASE_PATH}</code> &nbsp; Content: <code>{CONTENT_DIR}</code></p>
<form method="post" action="/admin/regenerate" style="margin:1rem 0"><button type="submit">Regenerate content files</button></form>
<table><thead><tr><th>MAC</th><th>Last Seen UTC</th><th>IP</th><th>Calls</th></tr></thead><tbody>{body}</tbody></table>
<h2>sum.txt</h2>
<pre>{sum_text}</pre>
<h2>manifest.txt</h2>
<pre>{manifest_text}</pre>
</body></html>"""
    return Response(html, mimetype="text/html")


@app.get("/")
def root() -> Response:
    return Response('<a href="/admin/">CYD Banners admin</a>', mimetype="text/html")


@app.post("/admin/regenerate")
def admin_regenerate():
    converted, content_sum = prepare_content()
    print(f"Admin regenerated content: converted images {converted}, sum {content_sum}")
    return redirect(url_for("admin"))


if __name__ == "__main__":
    print(f"CYD Banners server: http://{HOST}:{PORT}/")
    print(f"Update base path: {BASE_PATH}")
    app.run(host=HOST, port=PORT)
