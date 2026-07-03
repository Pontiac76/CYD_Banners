# CYD Banners — Project Outline

## Helper sites:
### ESP Connect 
URL: https://thelastoutpostworkshop.github.io/ESPConnect/
Allows to obtain information about the ESP32 via Chrome via a serial connection

### ESP32 Parition Builder
URL: https://thelastoutpostworkshop.github.io/ESP32PartitionBuilder/
Create the CSV for the partition style wanted for the Flash file system

### ESP32 Simulator
URL: https://wokwi.com/
Emulate common ESP32/Arduino/STM/Pico devices.

## Purpose

CYD Banners turns Cheap Yellow Display devices into small information/banner signs for retro LAN shows.

The displays are eye-candy and convenience devices, not required event infrastructure. They should be useful, resilient, and easy to recover, but they do not need live-floor debugging complexity.

## Core Model

- All CYDs run the same firmware.
- All CYDs receive the same public SD content tree.
- Each CYD identifies itself by ESP32 MAC address.
- The server maps MAC address to the device's current job/assignment.
- The CYD caches its last assignment locally so it can display something immediately at boot.
- SD content is non-sensitive and can be manually edited as a last-resort recovery path.
- LittleFS stores private/sensitive local configuration such as WiFi credentials and service tokens.

## Display Content

Initial display modes:

```text
IMG: path/to/image.cydimg
TEXT: path/to/text.txt
FILE: path/to/long_text.txt
QR: https://example.com
SCROLLIMG: path/to/rendered_scroll.cydimg
```

Rules:

- `IMG` shows only the image.
- `TEXT` shows formatted/static text.
- `FILE` may page or scroll longer text.
- `QR` shows the QR code and destination link text.
- `SCROLLIMG` is a server-rendered RGB565 scroll strip or equivalent generated asset.
- Blank lines and `#` comments in playlists are ignored.
- Unknown/bad instructions should be skipped safely and reflected in status.

## Text Formatting

Text files use a small BBCode-style subset.

- Default foreground: light gray.
- Default background: black.
- DOS-style 16-color names are used, e.g. `[red]`, `[green]`, `[white]`, `[lightred]`.
- Color applies until reset/end/new color; no nested color stack is required.
- Background can be set with tags such as `[bg=red]`.
- Palette values are runtime variables and may be overridden by server/manifest config.
- Bright color defaults should use true min/max RGB values where expected, e.g. `lightred = #ff0000`.

## Images

- Original JPG/PNG files are rendered/converted on the server, not on the CYD.
- CYD image assets should be in an efficient RGB565-style format, likely with a small self-describing header.
- Exact `.cydimg` format is still to be defined and benchmarked.

## WiFi / Network

- WiFi is useful but not mandatory for display operation.
- Device should render existing SD content as soon as possible.
- WiFi config has two private LittleFS-backed sources:
  - factory/baseline profiles flashed by PlatformIO
  - server-downloaded/event profiles
- Firmware should keep cycling/retrying known APs over time rather than giving up.
- Expected APs include Stephen's phone hotspot, home LAN, and portable event router.
- There is no NTP/current-time requirement. Slide timing uses uptime/timers.

## Update Model

- Server periodically generates public content files and manifest/checksum data.
- CYD periodically checks server `sum.txt` as a cheap change detector.
- If `sum.txt` changed, CYD downloads fresh `manifest.txt` and restarts update planning.
- Required files for the CYD's current assignment are prioritized first.
- Missing/changed files are downloaded individually, verified, and accepted.
- After required content is handled, CYD can perform restrained whole-manifest repair/audit:
  - quick file-size checks
  - limited one-file-at-a-time repair downloads
  - slow resumable checksum audits between screens
- If `sum.txt` changes during audit/repair, pending queues are wiped and planning restarts.

## Status / Floor Behavior

- Normal display may include a subtle 2–3 pixel bottom progress/status bar.
- Proposed status bar colors:
  - green: last call-home/check good
  - yellow: connected/check had a problem
  - white: hunting/scanning/trying APs
  - red: cycled the AP list since last success and still not connected
- Long press shows health/status details only.
- Do not use touch to change assignments during show use.
- Status should include current assignment, SD state, WiFi/AP state, IP if connected, call-home result, retry counters, and update/audit state.

## Failure Behavior

- If assigned content is missing, show an error/status and expedite network/update attempts.
- If SD is missing/unreadable, show an obvious local error.
- Avoid rapid flashing due to sensory/accessibility concerns; prefer solid red and/or slow gentle backlight pulse for severe errors.
- Normal show brightness should be 100%; no ambient dimming needed.
- If a unit is badly broken on the floor, it can be boxed and investigated later.

## Current Firmware State

The current firmware is intentionally bare-bones after removing stale LeftOvers code. It initializes display, LittleFS, SD, shows a POST/status shell, and draws a placeholder bottom progress bar. Renderer, update engine, WiFi manager, text parser, image format, and status screen still need to be implemented.
