from __future__ import annotations

from html import escape
from pathlib import Path
from typing import Any

TEMPLATE_PATH = Path(__file__).resolve().parent / "templates" / "admin.html"


def render_status_items(items: list[str], empty: str) -> str:
    if not items:
        return f"<li>{escape(empty)}</li>"
    return "".join(f"<li>{escape(item)}</li>" for item in items)


def apply_template(template: str, values: dict[str, str]) -> str:
    for key, value in values.items():
        template = template.replace(f"%%{key}%%", value)
    return template


def render_admin_page(
    *,
    base_path: str,
    content_dir: Path,
    host: str,
    port: int,
    token: str,
    shutdown_enabled: bool,
    sum_text: str,
    manifest_text: str,
    conversion_settings: dict[str, Any],
    content_status: dict[str, list[str]],
) -> str:
    shutdown_form = ""
    if shutdown_enabled:
        shutdown_form = '<form method="post" action="/admin/shutdown" style="margin:1rem 0" onsubmit="return confirm(\'Shutdown server?\')"><button type="submit" style="color:red">Shutdown</button></form>'

    values = {
        "BASE_PATH": escape(base_path),
        "CONTENT_DIR": escape(str(content_dir)),
        "HOST": escape(host),
        "PORT": str(port),
        "TOKEN": escape(token),
        "SUM_TEXT": escape(sum_text.strip()),
        "MANIFEST_TEXT": escape(manifest_text),
        "AUTO_CONTRAST_CHECKED": " checked" if conversion_settings["auto_contrast"] else "",
        "DITHER_CHECKED": " checked" if conversion_settings["dither"] else "",
        "BRIGHTNESS": escape(str(conversion_settings["brightness"])),
        "CONTRAST": escape(str(conversion_settings["contrast"])),
        "SATURATION": escape(str(conversion_settings["saturation"])),
        "GAMMA": escape(str(conversion_settings["gamma"])),
        "SHUTDOWN_CHECKED": "checked" if shutdown_enabled else "",
        "SHUTDOWN_FORM": shutdown_form,
        "WARNING_COUNT": str(len(content_status["warnings"])),
        "WARNING_ITEMS": render_status_items(content_status["warnings"], "No content warnings found."),
        "INFO_COUNT": str(len(content_status["info"])),
        "INFO_ITEMS": render_status_items(content_status["info"], "No orphan/info items found."),
        "PLAYLIST_COUNT": str(len(content_status["playlists"])),
        "PLAYLIST_ITEMS": render_status_items(content_status["playlists"], "No playlists parsed."),
    }
    return apply_template(TEMPLATE_PATH.read_text(encoding="utf-8"), values)
