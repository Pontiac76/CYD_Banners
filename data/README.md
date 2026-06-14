# LittleFS Data Files

Files in this directory are uploaded to LittleFS by PlatformIO when building/uploading the filesystem image.

Do not commit real private files such as:

- `wifi.txt`
- `config.txt`

Commit only `.sample` files with fake values.

Non-secret fallback/recovery files may be committed. Current public fallback:

- `Banners/error.txt` — shown from `LFS://Banners/error.txt` if SD/Banners content is unavailable.
