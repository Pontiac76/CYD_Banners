# CYD Banners Server

Minimal local Flask server for CYD Banners content/update testing.

## Windows setup

From the repo root:

```cmd
server\scripts\setup_venv.cmd
server\scripts\run_server.cmd
```

Default local port: `8088`.

Admin page:

```text
http://localhost:8088/admin/
```

## Config

Copy/edit `server/config.ini.sample` to `server/config.ini`. The setup script does this automatically if missing.

Only `sum.txt` currently requires the token. The token is read from the repo-local LittleFS source file `data/config.txt` so there is one token source for both firmware upload data and the local server.

```text
http://localhost:8088/cyd-banners/sum.txt?t=TOKEN&mac=AA:BB:CC:DD:EE:FF
```

Manifest and file download endpoints are open LAN endpoints:

```text
http://localhost:8088/cyd-banners/manifest.txt
http://localhost:8088/cyd-banners/files/playlist.ini
```

## Content

Published CYD content lives under:

```text
server/content/
```

This maps to the CYD SD card base directory:

```text
SD://banners/
```

So `server/content/playlist.ini` becomes `SD://banners/playlist.ini`.

`manifest.txt`, `sum.txt`, and converted `.cyd` image assets are regenerated from the `/admin/` page button or with the prepare script. `sum.txt`/`manifest.txt` endpoints only auto-generate if the files are missing.

To fully prepare content manually, including converted images and indexes:

```cmd
server\scripts\prepare_content.cmd
```

To regenerate only the indexes manually:

```cmd
server\scripts\generate_indexes.cmd
```

`jpg`, `jpeg`, and `png` source images are excluded from `manifest.txt`; CYDs should pull converted `.cyd` assets instead.

To convert source images to CYD-ready RGB565 `.cyd` files:

```cmd
server\scripts\convert_images.cmd
```

The converter scans `server/content/` for `.jpg`, `.jpeg`, and `.png` files and writes sibling `.cyd` files. Default output is 320x240 fit/letterbox to preserve the full source image. Use `--mode cover` to crop/fill instead. A sibling `.cyd.meta.json` tracks source hash and conversion settings so images are regenerated when size/mode/source content changes.
