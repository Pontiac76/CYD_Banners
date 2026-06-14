# Project Memory — CYD Banners

This file is the project memory bank for future AI-agent sessions. Read it at the start of work and update it when durable project facts or decisions change.

## How to Use This Memory

- Keep entries concise, factual, and useful for future coding sessions.
- Prefer updating existing bullets over appending duplicates.
- Record decisions, user preferences, hardware assumptions, gotchas, and current implementation status.
- Do not store secrets such as WiFi passwords, private URLs, API keys, or session tokens.

## Project Identity

- Working project: `Banners`.
- Purpose: Cheap Yellow Display information/banner signs for Stephen's retro LAN shows.
- Stephen decided to keep CYD projects separate rather than combine `Banners`, `Clock`, and `LeftOvers` into one repo/project. No GitHub push has happened yet, so repo layout/history can still be reshaped.
- Stephen runs a LAN retro show a couple of times per year.
- Stephen has about 9 CYD devices; roughly 4–6 may be used at an event.
- Each CYD can be assigned/tagged to a retro machine or show station.
- Displays may show machine-specific information plus event tidbits: specs, descriptions, Discord QR codes, QR codes to richer web pages, images/logos/screenshots, and other show information.
- This project is being created from the code currently in this directory, which came from `CYD_LeftOvers`. Existing LeftOvers subject matter, file names, structure, and code are sacrificable.

## Repository / Sibling Projects

- Current working directory: `C:/git/CheapYellowDisplay/Banners`.
- Current Git root is `C:/git/CheapYellowDisplay`, but Stephen intends to modify this so the CYD projects stay separate.
- Sibling CYD projects one directory up include:
  - `../Clock` — original CYD clock project, incomplete/WIP.
  - `../LeftOvers` — leftovers display project, incomplete/WIP.
- It is OK to read and borrow code/ideas from sibling projects.
- Do not modify sibling projects unless Stephen explicitly asks.

## Hardware / Build Assumptions

- Target board: `esp32-2432S028Rv3` Cheap Yellow Display.
- Framework: PlatformIO Arduino, env currently `cyd`.
- Filesystem: LittleFS, custom partition file `no_ota_littlefs.csv`.
- Current LittleFS partition is `0x080000` bytes = 512 KiB, not 512 MiB.
- LittleFS is intended for sensitive/local device data such as WiFi credentials and private config, not displayed media assets.
- SD card and LittleFS are both expected to be used for different purposes: SD for non-sensitive display content/assets; LittleFS for secrets/private local config so removing the SD card does not expose network credentials.
- Display/touch: ILI9341 via TFT_eSPI and XPT2046 touch.
- Important CYD pin/build definitions live in `platformio.ini`.
- Existing useful hardware pieces include TFT setup, QR drawing, touch handling, WiFi STA/setup portal, LittleFS helpers, and brightness/photoresistor handling.

## Current Codebase Survey

Reusable or worth recycling:

- `src/main.cpp` — boot sequence, TFT init, touch init, WiFi/NTP startup, loop dispatch.
- `display_manager.*` — TFT global, color helpers, build code, QR rendering, setup/status screens.
- `touch_manager.*` — XPT2046 HSPI setup and touch polling.
- `storage_manager.*` — LittleFS/optional SD text-file helpers and file listing.
- `setup_portal.*` — AP/captive portal flow and `/wifi.txt` writing.
- `network_manager.*` — STA connect, explicit UDP NTP query, sync scheduling, timezone handling.
- `config_manager.*` — key/value config parsing, colors, brightness, NTP settings; contains legacy clock/system-id pieces.
- `brightness_manager.*` — backlight and photoresistor dimming logic.
- `leftovers_web.*` — reusable patterns for Arduino `WebServer`, generated HTML, QR/token-gated actions, and admin unlock flow.

Mostly disposable/domain-specific:

- `leftovers_*` domain names, data model, screens, and web UI.
- `schedule_display.*`, unless recurring schedule/time-rule logic becomes useful.
- Legacy names/settings such as `CYD-Clock-Setup`, `LeftOvers`, `schedule`, `system_id`, and `updateurl` should be cleaned up as the new project structure is defined.

## Security / Git Hygiene

- Treat security implications seriously even though this is a local/event display project.
- Sensitive real files such as `data/wifi.txt` and private `data/config.txt` must not be committed to GitHub.
- Commit sample files such as `data/wifi.txt.sample` and `data/config.txt.sample` with fake/unprocessed/example values only.
- Passwords or private URLs belong only in ignored local LittleFS-backed config files, not source code, memory, docs intended for GitHub, committed assets, or SD display content.
- Assume someone might remove/steal an SD card from a CYD on the show floor; SD content should be non-sensitive.
- `.gitignore` should ignore real local secrets while allowing sample files.

## Expected Project Direction

Likely firmware responsibilities:

- Load sensitive/private config from LittleFS and non-sensitive display content/assets from SD.
- Render one or more information pages/slides in portrait or landscape as chosen later.
- Show text, simple layout blocks, QR codes, and converted image assets.
- Support per-device assignment/configuration for different machines/stations.
- Keep useful local operation even if WiFi is unavailable.
- WiFi setup/config should support multiple access points. Expected profiles include Stephen's phone hotspot, home LAN, and the portable event router. Only one AP needs to connect at a time, but firmware must keep cycling/retrying known APs over time rather than giving up after the last entry.
- WiFi config should have two LittleFS-backed sources: a factory/baseline file flashed by PlatformIO with known stable networks, and a server-downloaded file that can add/update event-specific networks discovered later. The baseline should remain a recovery path and not be casually destroyed by server updates.
- WiFi resiliency matters: APs may disappear/reappear during an event (phone hotspot sleeps, router/cellular uplink changes, local WiFi varies). CYDs should maintain display operation and periodically retry finding/connecting to known networks.
- The project does not require current date/time or Internet NTP. Slide/page timing can use `millis()`/timers; NTP should not be part of the required startup path unless a future feature needs real time.
- Touch behavior direction: long press shows local status/identity; status screen can include device ID, IP, SSID, content pack, and an Admin button.
- Admin QR flow can reuse the LeftOvers pattern: physically request admin on the CYD, generate/show a QR/token, and let the phone open the admin page for that specific device.
- Content update flow can reuse ideas from the Clock update function: configured update-source URLs, request asset manifest/list/checksums, compare SD files, and pull changed/missing content files.
- Tar packages are acceptable/preferred over ZIP if an archive package is used, but a manifest-plus-individual-files update model may be better. HTTP request count is not a major concern because the server will be Stephen's machine and content should be modest.
- Update server should provide already-rendered CYD assets, not original JPG/PNG source files. Image conversion happens server-side/host-side, not on the CYD.
- CYD display modes under consideration: QR mode shows a QR code plus the destination link text; image mode shows only images; text mode shows only text; file/scroll mode may scroll a text file upward one scanline at a time.
- Text markup should be a practical BBCode-style subset. Color tags are DOS 16-color-style names such as `[red]`, `[green]`, `[white]`, `[lightred]`; default foreground should be light gray. Color applies until reset/end/new color, without needing a nested color stack. Background can be set with a tag such as `[bg=red]`.
- Color definitions should be mutable runtime variables, not compile-time constants, so server/manifest config can override palette values for lighting/display differences or special effects. Default bright colors should use true min/max RGB values where expected, e.g. `lightred = #ff0000`, not softened/shaded approximations.
- Palette changes need an explicit reload/apply path. This may be handled as manifest metadata or file-change event behavior; if palette changes, rendered text/screens should use the new palette without requiring firmware changes.
- In docs/examples/discussion, prefix paths with storage source when relevant: `SD://path` for SD card content and `LFS://path` for LittleFS/private config, to keep data origin clear. Firmware variable names can still make this clear in code without literal prefixes.
- Each CYD project should use an SD base directory named after the project. For Banners, firmware should require/use `SD://Banners/` and work relative to that directory.
- Playlist design direction: root file is always `SD://Banners/playlist.ini`. It is INI-style only for grouping: `[GLOBAL]` contains common startup/show/sponsor slides rendered by every CYD first; a MAC-specific section like `[AA:BB:CC:DD:EE:FF]` contains that device's machine/station-specific slides rendered after global content; optional `[DEFAULT]` is used if no MAC section exists. Lines inside sections are file paths, not `KEY: value` instructions; responsibility is determined by extension.
- File extension definitions: `.ini` = root INI grouping file (`SD://Banners/playlist.ini`) and may also be accepted as an include/list file while hand-authored content evolves; `.play` = playlist/include file containing a simple list of files to render/include; `.txt` = text content rendered to screen; `.qr` = QR-code content file (format TBD after text rendering is firmer); `.cyd` = CYD-ready image asset, likely RGB565-style. Playlist/`.play`/included `.ini` parsing must trim whitespace around each path/comment line, including trailing spaces humans may accidentally leave. Rendered `.txt` content lines themselves should not be globally trimmed because whitespace may be intentional, except syntax prefixes such as `1:`/`2:` can parse their marker separately. CYD builds an expanded in-RAM runtime playlist and display playback walks that RAM list rather than recursively reading SD during rendering.
- Playlist paths may be relative to the containing playlist file, absolute from SD root with a leading `/`, or source-prefixed. `SD://Banners/foo.txt` resolves to SD absolute path `/Banners/foo.txt`. `LFS://...` is reserved for LittleFS/private config references; rendering LFS content is not implemented yet. Avoid generating doubled prefixes such as `SD://Banners/SD://...`.
- `PLAYLIST:` behaves like an include expanded in place, but each unique playlist file should only be processed once per reconstruction. If encountered again, skip/reuse prevention avoids circular/self-recursive infinite loops. The final renderable runtime playlist can contain repeated display items if authored directly, but playlist source files are only parsed once by canonical path.
- Firmware should read `SD://Banners/playlist.txt` as the root playlist/mapping line-by-line and generate an in-memory list of display items. Root playlists may include common event/show slides first (logo image, titled QR, sponsors playlist), then machine/floor-specific details.
- Firmware should keep a lightweight pulse on relevant SD files for the active runtime playlist. If `SD://Banners/sum.txt` changes due to background update/sync, keep displaying the current RAM playlist while update/download work completes. After the update is done, re-read `SD://Banners/playlist.txt`, rebuild/reprocess the runtime playlist, then start playback again from slide 1.
- Missing referenced files/assets are non-fatal: print each missing file event to Serial, skip the item, and continue processing/playback. Maintain a small rolling in-RAM missing-file list across rebuilds/passes rather than clearing it every rebuild. When a missing file is discovered, append it if not already tracked, trimming oldest entries to keep roughly 5–10 items. When a later pass finds that a tracked file now exists, remove it from the missing list. CYD info/status should show this clean current rolling list/count rather than permanent history.
- Rendering should avoid visible flicker. Prefer drawing static/full-page content into an off-screen buffer/sprite or cached page representation, then pushing to the TFT once; avoid clearing/redrawing the whole visible screen for small status updates. Stephen expects enough RAM for full-screen sprites/temp buffers on these CYDs as long as temporary sprites are cleared/deleted when no longer needed, so do not over-optimize around RAM concerns prematurely.
- Text/file display direction: content can be paged instead of continuously smooth-scrolled. Calculate available text lines per page from font/line height and viewport; e.g. 30 lines at 10 visible lines becomes 3 pages. Text may be composited over a background image into an off-screen page, pushed once, then background tasks/progress bar can run during the dwell period.
- Screen transitions may optionally fade between screens so updates occur while the display is effectively off; normal brightness remains 100%.
- Update sources may be Internet-accessible via phone/router, local WiFi, or a laptop/docker-hosted local web server.
- A private random/service-account request string may identify authorized update requests; this secret belongs in LittleFS/private config, not on SD or in Git.
- Device identity should be distinct from show assignment. The ESP32 MAC address can be the authoritative unique device identity; the server can map MAC/device ID to a show role/content target such as `486`, `P3`, etc. Friendly labels like `CYD-01` may still be useful for humans/status screens, but server assignment can be MAC-based.
- Stephen's home network/IPAM requires devices to be statically registered by MAC address. Unknown MACs are placed into a non-routed/router-blocked DHCP range. Onboarding a new CYD means reading the DHCP-discovered MAC, registering/static-assigning it in IPAM/DHCP, rebooting the CYD, and validating the expected IP. Therefore Stephen will know each CYD MAC before the banner server needs to assign metadata/content.
- The banner/update server can hold all per-MAC metadata needed for CYD display/function after network onboarding: friendly label, assigned content path, update policy, status expectations, etc.
- Content direction: one shared SD content tree/file set for all CYDs. PlatformIO builds/flashes the same firmware/LittleFS baseline regardless of which device will display which machine/station.
- Each device renders only the local path/playlist assigned to it by the server. The last assigned job/path should be persisted locally so the CYD knows what to display immediately at boot, even before it talks to the server again. SD is likely preferred for this responsibility so Stephen can manually edit the assignment on the SD and reboot as a last-resort/network-free recovery.
- Reassigning a CYD to another machine should usually mean changing the assigned local content path/job from the server, not redownloading everything, assuming the shared content set is already present on SD.
- Update direction favors server-provided `sum.txt` plus `manifest.txt` and individual file pulls over archive packages. Server periodically recomputes manifest/checksums. CYDs use `sum.txt` as the cheap change detector; when it changes, download fresh manifest and restart update/audit planning.
- Proposed sync model: first identify/download files immediately required for the CYD's current assigned job/path. A compare function builds a download list and feeds a downloader that can run in immediate mode or limited/background mode. Downloader verifies downloads before accepting files.
- After required files are handled, scan the manifest top-to-bottom for quick file-size mismatches. Size mismatch/missing files are queued for limited repair downloads, one file per update opportunity; failures move to the bottom of the queue.
- Once size-mismatch queue is empty, perform slow checksum sanity checks one file at a time between screen renders. Checksum mismatches are queued for limited redownload. Audit should be resumable by file path, last read offset, expected size/hash, and an incremental hash context. Each audit slice may open/seek/read/close a chunk, update the hash context, and return.
- If `sum.txt` changes, a file is replaced, or update planning invalidates current assumptions, safely cancel/nuke any active audit task and pending queues as needed. `resumeAudit` should only run when a valid active task exists; otherwise it should be skipped/no-op.
- During assembly, use the ESP32 MAC as an automatic unique fallback/primary ID. Fresh/unassigned devices can call home as unassigned and be assigned from the server.
- Admin/status mode is expected to be mostly status/diagnostics rather than a full direct-control UI, because the update/content server can tell each CYD what it needs to do.
- Normal display may include a subtle bottom progress/status bar instead of a separate icon: 2–3 pixels tall along the bottom of the screen, advancing during the current slide wait period and colored by network state. Proposed colors: green = last call-home/check good; yellow = connected/check had a problem; white = currently hunting/scanning/trying APs; red = cycled through the full known AP list at least once since last successful connection and still not connected.
- Long press on the screen shows health/status details only. Do not use touch to change jobs/assignments during normal show use; nearby attendees could accidentally change displays, and the server should remain authoritative when network is available.
- Status mode should show what the device is currently trying to do: current/target WiFi network, IP if connected, call-home/update target status, live call-home test result, current content/version, and retry/cycle counters per AP where practical. This helps diagnose bad network areas on the show floor.
- Startup should begin rendering existing SD content as soon as possible if present. Network checks/update attempts should happen opportunistically during slide/page wait periods or other safe idle moments.
- If assigned content is missing, this should normally be caught during home testing. On-device behavior: show the problem in status/error display and expedite attempts to connect to a valid AP/update source.
- If network/update fails, the CYD should keep using existing local content. Last-resort recovery can be manual SD copy/content replacement.
- If SD is missing/unreadable, show an obvious local error. Avoid rapid flashing due to sensory/accessibility concerns; prefer a solid red error screen and/or slow, gentle backlight fade/pulse to attract attention.
- At shows, normal LCD brightness should be 100%; no ambient dimming needed for normal operation. Brightness changes may be used only for special error indication if safe/subtle.
- If config/assignment is invalid, fall back to a default/safe display path or clear unassigned/error page rather than crashing or blanking. This should be rare after testing.
- Factory reset/recovery gestures are low priority; if a unit is badly broken on the floor, Stephen is likely to box it and investigate later rather than debug live.
- Firmware version complexity is low priority. Stephen expects to flash all needed CYDs before the show and run one firmware build; these displays are fun/eye-candy/party-favor devices, not required production tooling.

Likely host-side tooling responsibilities:

- Convert JPG/PNG images into CYD-displayable assets.
- Possibly build/upload a LittleFS content pack.
- Possibly generate per-device configs or page definitions.

## Image Asset Notes

- A conversion tool is needed somewhere/somehow to take JPGs or PNGs and produce assets that can be shown efficiently on the CYD.
- Exact format is not decided yet. Options may include RGB565 raw/bin, generated C arrays, or a simple indexed/RLE format if storage or speed needs it.
- Prefer a practical pipeline that makes event prep easy over a theoretically perfect graphics system.

## User Preferences / Tone

- Stephen values practicality and scope control.
- Existing code may be sacrificed for the new project; do not preserve old LeftOvers behavior unless it is useful.
- Avoid over-architecting before the banner/info-display workflow is clear.
- Be careful not to touch sibling projects unintentionally.
- Treat this as two equal developers working the problem. Stephen has final override and go/no-go authority, but welcomes constructive questions, advice, suggestions, and alternatives.
- Question anything that seems questionable, inefficient, risky, or overcomplicated, but be constructive and discuss alternatives before applying them.
- If Stephen gives the green light after discussion, apply the agreed approach.

## Current Memory Setup

- `AGENTS.md` tells future agents to read and maintain this file.
- `MEMORY.md` may be committed to GitHub for shared project context.

## Current Firmware State

- Stale LeftOvers modules have been removed from this working project.
- WiFi startup now reads `LFS://wifi.txt` INI-style profiles, cycles connection attempts, reads private update config from `LFS://config.txt`, and uses HTTP `GET <update_source>/sum.txt` as a basic call-home probe. Status bar colors now reflect network/update health: green = connected/call-home OK, yellow = connected but token/source/call-home problem, white = hunting, red = at least one full WiFi profile cycle without connection.
- Initial Flask-based CYD Banners server lives under `server/`, defaulting to port 8088. It serves one published content location (`server/content/`), which maps to CYD SD base `SD://banners/` (e.g. `server/content/playlist.ini` -> `SD://banners/playlist.ini`). It regenerates `manifest.txt`/`sum.txt` at runtime, requires the token only for `GET /cyd-banners/sum.txt?t=...&mac=...`, records call-home heartbeats in ignored `server/state/heartbeats.json`, and provides a simple `/admin/` last-seen page. Windows helper scripts are `server/scripts/setup_venv.cmd` and `server/scripts/run_server.cmd`.
- Current firmware in `src/main.cpp` initializes TFT, touch, LittleFS, SD, parses `SD://banners/playlist.ini`, expands playlist includes into an in-RAM slide list, renders basic `TEXT:` slides with `1:`/`2:` heading markers, renders `QR:` slides, accepts `.cyd` image entries but currently shows only an image-renderer placeholder, and skips missing image/files while tracking a rolling missing-file list for the touch info screen. Full-screen sprite allocation failed on current hardware/runtime, so current flicker control uses direct full-screen draws only on slide/screen changes plus incremental progress bar drawing. Update flow now compares remote `sum.txt`, fetches `manifest.txt` when changed, plans deltas by size then MD5, downloads the root playlist first if needed, rebuilds the runtime playlist, then downloads changed/missing files needed by the current playlist before rechecking health and writing `SD://banners/sum.txt`.
- `OUTLINE.md` has been rewritten for CYD Banners.
- `data/wifi.txt.sample`, `data/config.txt.sample`, and `data/README.md` exist as safe LittleFS examples. Real `data/wifi.txt` and `data/config.txt` remain ignored.
- Renderer, WiFi manager, update engine, status screen, BBCode parser, image asset format, and playlist handling are not implemented yet.

