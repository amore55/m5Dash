# Tab5 Desk Dashboard

A self-contained touchscreen desk dashboard for the **M5Stack Tab5** (ESP32-P4). It connects
straight to Wi-Fi, runs its whole interface locally, and shows full-screen pages you move between
by swiping.

No hub, no broker, no companion app, no cloud account. The device talks to public APIs directly
and keeps working when any one of them doesn't.

Pages: **Clock · Weather · Elizabeth line · To-dos · Claude usage**, plus **Settings** opened by
a long press, outside the normal rotation.

---

## ⚠️ Status: early. Read this before expecting it to work.

The foundation is complete and **compiles cleanly** for the ESP32-P4. Most of the integrations
are not written yet, and **nothing has ever been run on hardware.**

| Area | State |
| --- | --- |
| Build system, partitions, versioning | ✅ Done — `idf.py build` exits 0, no warnings in project code |
| Board bring-up (display, touch, LVGL, backlight, RTC) | ✅ Written, **never run on a device** |
| Plugin framework, page manager, gestures, theme | ✅ Done |
| **Clock page** | ✅ Real, working code |
| Weather · Elizabeth line · To-dos · Claude · Settings | 🔲 Placeholder pages only |
| Wi-Fi, NTP, first-run setup portal | 🔲 Not started |
| Storage (settings, tasks, cache) | 🔲 Not started |
| OTA updates | 🔲 Architecture and partitions ready, service not written |
| CI workflows, host unit tests | 🔲 Not started |

Current build: **~1.24 MB**, against a 6 MB OTA slot — 80 % free.

Work in progress is tracked in [`docs/BACKLOG.md`](docs/BACKLOG.md), which is the file to read if
you are picking this up cold.

---

## Hardware

| | |
| --- | --- |
| Board | [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5) |
| Application processor | ESP32-P4 (dual-core RISC-V) |
| Wireless | ESP32-C6 coprocessor over SDIO (the P4 has no radio of its own) |
| Display | 5″ 1280 × 720 IPS via MIPI-DSI — **physically 720 × 1280 portrait**, rotated in software |
| Touch | GT911 or ST7123 depending on hardware revision, auto-detected |
| RTC | Epson RX8130CE on I²C `0x32` |
| PSRAM / Flash | 32 MB / 16 MB |
| Power | USB-C |

You also need a USB-C **data** cable. No USB driver is required — the Tab5 uses the ESP32-P4's
native USB, so Windows enumerates it as a standard COM port.

## Software

**ESP-IDF v5.4.4** and nothing else. It brings its own Python, compiler, CMake, Ninja and esptool.

> **Why 5.4.4 and not 5.4.2?** M5Stack's reference project states 5.4.2, but Espressif's Windows
> installer offers no such version — only patch-release archives (5.5.5, 5.4.4, 5.3.5, 5.2.7) or
> git clones of release branches. Reaching 5.4.2 exactly means a ~2.5 GB clone plus four manual
> steps on every machine. 5.4.4 is the same minor line, is API- and ABI-compatible, and keeps
> local builds identical to CI — which is the property that actually prevents bugs. Full
> reasoning in [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md) §2.1.

---

## Quick start

```powershell
git clone https://github.com/amore55/m5Dash.git
cd m5Dash

# Activate ESP-IDF. Note the leading dot-space: this MUST be dot-sourced.
. .\scripts\idf_env.ps1

idf.py set-target esp32p4      # first time only
idf.py build

# Put the Tab5 in download mode: hold Reset ~2 s until the green LED blinks rapidly
[System.IO.Ports.SerialPort]::GetPortNames()          # find your port
esptool.py --chip esp32p4 -p COM7 flash_id            # CONFIRM 16MB before flashing
idf.py -p COM7 flash monitor                          # Ctrl-] to exit the monitor
```

**Full step-by-step, including troubleshooting: [`docs/FLASHING.md`](docs/FLASHING.md).**

### Why `scripts/idf_env.ps1` rather than ESP-IDF's `export.ps1`

ESP-IDF's own `export.ps1` invokes a bare `python`, which on Windows 11 resolves to the Microsoft
Store app-execution-alias stub. The stub exits without doing anything and `export.ps1` then fails
with a confusing PowerShell parse error, leaving `IDF_PATH` empty. It looks like a broken ESP-IDF
installation and is not. The helper calls the ESP-IDF virtualenv's `python.exe` by absolute path.

The Start-menu *ESP-IDF 5.4.4 PowerShell* shortcut also works fine.

---

## Repository layout

```
main/                     app_main + boot order; placeholder pages
components/
  tab5_board/             thin wrapper over Espressif's official Tab5 BSP:
                          bring-up, 90° rotation, RAII LVGL lock, backlight,
                          RX8130CE RTC driver, ESP32-C6 power rail
  dashboard_core/         plugin interface, PluginBase, worker tasks, PageManager,
                          gesture detection, theme, page scaffold, time/JSON utilities
  dashboard_network/      (planned) Wi-Fi, SNTP, HTTPS client, setup portal
  dashboard_storage/      (planned) settings + migrations, LittleFS, task store, cache
  dashboard_ota/          (planned) manifest parsing, OTA with SHA-256 and rollback
plugins/                  one ESP-IDF component per page
  clock/                  ✅ implemented
  weather/ elizabeth_line/ todos/ claude/ settings/   (planned)
config/                   version, policy constants, placeholder example config
docs/                     plan, backlog, flashing, OTA, configuration, Claude notes
scripts/                  ESP-IDF activation helper, release tooling
```

`plugins/` is on `EXTRA_COMPONENT_DIRS`, so **every page is a real ESP-IDF component**. Removing
one from the firmware is a one-line change in `main/CMakeLists.txt` plus deleting its directory.

### Built on Espressif's official BSP

Board support comes from [`espressif/m5stack_tab5`](https://components.espressif.com/components/espressif/m5stack_tab5),
not from a hand-port of M5Stack's demo. That BSP handles both Tab5 hardware revisions
(ILI9881C + GT911, and the newer ST7123 for both display and touch) with automatic detection, and
targets LVGL 9. The one piece of low-level hardware access written here is the RX8130CE RTC
driver, because the BSP does not expose the RTC.

---

## How it works

### The threading contract

This is the rule the whole application hangs off, and it is what keeps the UI responsive:

* `createPage()`, `onShow()`, `onHide()`, `tick()` and `refresh()` all run on the **LVGL thread**,
  with the LVGL lock already held.
* **`refresh()` must not do work.** It may only post a job to the plugin's own worker task.
* HTTPS, JSON parsing and filesystem access happen on that **worker task**, which never touches
  an `lv_obj_t`.
* Handoff is a mutex-guarded model plus an atomic dirty flag. `tick()` is where widgets change.

A plugin that breaks this doesn't just slow itself down — it blocks every other page, because
there is one LVGL thread. `PluginBase` implements the contract so no plugin has to re-derive it.

### Pages are built once

`PageManager` creates every page's widget tree at start-up and navigates by toggling
`LV_OBJ_FLAG_HIDDEN`. This costs memory (comfortably affordable with 32 MB of PSRAM) and makes
"no leaks when pages are shown repeatedly" a structural guarantee rather than something each
plugin must remember.

### Gestures are polled, not evented

Navigation does **not** use `LV_EVENT_GESTURE`. LVGL delivers gesture events to the object under
the finger, so any button or list row swallows them — navigation would silently stop working
exactly where the UI is most interactive. Instead the input device is polled on a 30 ms timer
with an independent state machine, and `lv_indev_reset()` is called when a gesture fires so a
swipe starting on a button doesn't also click it.

All thresholds live in [`config/app_config.hpp`](config/app_config.hpp).

---

## Configuration and secrets

**No credential is ever committed.** [`config/example_config.json`](config/example_config.json)
documents every configurable value using placeholders only, and `.gitignore` blocks local
configuration, build output and generated files.

Once the storage and network components exist, configuration will live in NVS and be set through
a first-run setup portal (the device raises an access point called `DeskDashboard-Setup`) or the
on-device Settings page. Secrets go in a separate NVS namespace, are masked in the UI, and are
never logged.

TLS certificate validation is on and stays on — `esp_crt_bundle` with the full root set.
Verification is never disabled to make something work.

See [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md).

---

## Documentation

| | |
| --- | --- |
| [`docs/BACKLOG.md`](docs/BACKLOG.md) | **Start here.** Current state, what's next, and every decision already settled |
| [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md) | Verified hardware facts, design decisions and open questions |
| [`docs/FLASHING.md`](docs/FLASHING.md) | Step-by-step USB flashing and troubleshooting |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Component and threading design *(planned)* |
| [`docs/OTA.md`](docs/OTA.md) | Over-the-air updates and recovery *(planned)* |
| [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) | Every setting, and how secrets are handled *(planned)* |
| [`docs/CLAUDE_USAGE.md`](docs/CLAUDE_USAGE.md) | Why the Claude page is experimental *(planned)* |

---

## Known limitations

1. **Nothing has been run on hardware.** The firmware compiles and links for esp32p4. That proves
   nothing about display output, touch, the RTC, the ESP32-C6 Wi-Fi link, or whether software
   rotation to 1280 × 720 is fast enough.
2. **Flash size is assumed, not confirmed.** `partitions.csv` assumes 16 MB. Run
   `esptool.py --chip esp32p4 -p COM7 flash_id` before trusting it.
3. **Large fonts are scaled, not native.** LVGL's built-in Montserrat stops at 48 px, so the
   clock's hero digits are enlarged with an LVGL transform and are slightly soft. A script to
   generate a crisp 160 px face from a redistributable font is planned.
4. **Displayed text is limited to the built-in font's glyph range** — printable ASCII, `°`, `•`
   and the `LV_SYMBOL_*` icons. Em dashes, ellipses and middle dots render as empty boxes.
   (`°C` is fine.) See [`LICENSES.md`](LICENSES.md#fonts-and-icons).
5. **The Claude usage page is experimental and may never work reliably.** Anthropic publishes no
   API for personal allowance usage. See `docs/CLAUDE_USAGE.md`.
6. **Gesture thresholds are first guesses** and have not been felt on real glass.
7. **NVS encryption is off by default.** It requires flash encryption, which is irreversible and
   complicates development flashing. Until enabled, someone with physical access and a USB cable
   can read stored secrets out of flash.

---

## Roadmap

1. ~~Build system, board support, plugin framework, clock~~ ✅
2. Storage: settings with schema migrations, LittleFS, task store, response cache
3. Network: Wi-Fi manager, SNTP with RTC fallback, HTTPS client, first-run setup portal
4. Public integrations: Elizabeth line (TfL), weather (Open-Meteo)
5. To-dos via Telegram long polling
6. OTA: manifest, SHA-256 verification, channels, automatic rollback
7. Claude usage (experimental, isolated so it cannot delay the rest)
8. Idle wallpaper / PIN lock screen
9. CI build and tagged-release workflows, host unit tests

---

## Licence

MIT — see [`LICENSE`](LICENSE). Third-party components and their licences are recorded in
[`LICENSES.md`](LICENSES.md).
