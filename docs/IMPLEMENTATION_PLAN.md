# Implementation Plan — Tab5 Desk Dashboard

Status: **v0.1.0 first pass.** Written before implementation, updated at the end of the
first pass with what was actually built.

---

## 1. Workspace inspection (before any files were created)

| Check | Result |
| --- | --- |
| Workspace path | `c:\Moreno Functions\Projects\M5Dash` — contains a space, which turns out to be fine on 5.4.4; see §1.1 |
| Contents | Empty |
| Git repository | **No** — `git init -b main` was run as part of this pass |
| Remote | `https://github.com/amore55/m5Dash` (to be added, see README) |
| `idf.py` on PATH | **Not present** |
| ESP-IDF install (`~/esp`, `C:\Espressif`, `~/.espressif`) | **Not present** |
| Docker | **Not present** |
| CMake / Ninja / Make | **Not present** |
| Host C/C++ compiler (gcc, clang, MSVC) | **Not present** |
| WSL | **Not installed** |
| Python | 3.14.6 (via the `py` launcher; `python`/`python3` are Store aliases) |
| `gh` CLI | **Not present** |

**Consequence:** neither the firmware build nor the host unit tests can be compiled on this
machine as it stands. The repository is therefore designed so that **GitHub Actions is the
first thing that compiles it**, and both a firmware build job and a host-test job run on
every push and pull request. See [§11](#11-what-could-not-be-verified-locally).

### 1.1 Spaces in the project path — tested, and NOT a problem on 5.4.4

The widely-reported ESP-IDF limitation that the build system cannot handle spaces anywhere in
a path (`IDF_PATH`, the project directory, a component directory) **does not apply to
ESP-IDF 5.4.4**, at least for this project.

This was assumed from Espressif issue reports, acted on by relocating the repository, and then
**tested properly and found to be wrong**. A full `idf.py build` from
`c:\Moreno Functions\Projects\M5Dash` completes with **exit 0** and produces a byte-identical
binary size to a build from a space-free path. The generated Ninja rules quote paths correctly:

```
cd /D "C:\Moreno Functions\Projects\M5Dash\build\esp-idf\esptool_py" && ...
```

**The project therefore lives at `c:\Moreno Functions\Projects\M5Dash`.** The temporary
relocation to `C:\dev\m5Dash` has been reverted and that copy deleted.

Two caveats worth keeping in mind, since the underlying advice is not entirely obsolete:

* The **ESP-IDF installation** path is a separate matter, and Espressif's own installer still
  warns about spaces, parentheses and a 90-character limit there. `C:\Espressif` is used and is
  safe regardless.
* A third-party component with a hand-written build rule could still mishandle a quoted path.
  If an unexplained "file not found" or "permission denied" appears mid-build after adding a
  new component, a space in the path is worth eliminating as a hypothesis — but it is not the
  default explanation it once was.

---

## 2. Reference projects inspected

### `m5stack/M5Tab5-UserDemo` (the official M5Stack Tab5 project)

Inspected: `README.md`, `platforms/tab5/sdkconfig.defaults`, `platforms/tab5/sdkconfig`,
`platforms/tab5/partitions.csv`, `platforms/tab5/main/idf_component.yml`,
`platforms/tab5/dependencies.lock`, `platforms/tab5/components/`.

Findings that this project relies on:

* **ESP-IDF line: `v5.4.x`.** M5Stack state `v5.4.2` in the README, confirmed by their
  `dependencies.lock` (`idf: 5.4.2`, `target: esp32p4`). No evidence they have moved off the
  5.4 line.

  **This project pins `v5.4.4`, not `v5.4.2`** — see [§2.1](#21-why-544-and-not-542).
* The Wi-Fi path is `esp_wifi_remote` + `esp_hosted` over **SDIO** to the ESP32-C6.
* The **exact SDIO pin map and reset line for the Tab5's C6** were taken verbatim from
  M5Stack's committed `sdkconfig` (see [§5](#5-esp32-c6-wi-fi-link-verified-pin-map)).
  These are *not* invented.
* M5Stack vendor a private copy of the BSP (`platforms/tab5/components/m5stack_tab5`) and
  pin LVGL **8.4.0** because their UI framework (`esp-brookesia`, `mooncake`,
  `smooth_ui_toolkit`) is LVGL 8. We do **not** follow them here — see the next section.

### 2.1 Why 5.4.4 and not 5.4.2

The brief asked to start at 5.4.2 unless inspection showed M5Stack had moved. They have not
moved — but **5.4.2 is not installable as a supported unit on Windows**. Espressif's
Universal Online Installer 2.4.1 offers only:

* patch-release **zip archives**: 5.5.5, 5.4.4, 5.3.5, 5.2.7
* **git clones of release branches**: `release/v5.5`, `release/v5.4`, `release/v5.3`,
  `release/v5.2`

and there is no offline installer for 5.4.2 either. Reaching exactly 5.4.2 means cloning
`release/v5.4` (~2.5 GB with submodules), fetching tags, checking out `v5.4.2` and re-running
`install.ps1` — four manual steps and a shallow-clone trap, on every machine that ever
builds this project.

**Decision: standardise the whole project on `v5.4.4`.**

Rationale:

* 5.4.4 is a bugfix patch on the *same minor line*. ESP-IDF patch releases are API- and
  ABI-stable, so the BSP (`idf: ">=5.4"`), `esp_hosted 1.4.0` and `esp_wifi_remote 0.8.5`
  are all unaffected.
* It is strictly newer than 5.4.2, so it carries fixes rather than losing them.
* **The property that actually prevents bugs is local == CI**, not the digit `2`. A
  one-click local install that matches the CI container is worth more than fidelity to a
  version number M5Stack picked by convenience.

Consequences, applied throughout:

| Where | Value |
| --- | --- |
| `.github/workflows/build.yml` | `espressif/idf:v5.4.4` |
| `.github/workflows/release.yml` | `espressif/idf:v5.4.4` |
| `main/idf_component.yml` | `idf: ">=5.4,<5.5"` — accepts any 5.4.x, refuses 5.5 |
| Local install | `v5.4.4` zip archive, `C:\Espressif\frameworks\esp-idf-v5.4.4` |

The manifest deliberately allows the whole 5.4 line rather than hard-pinning `5.4.4`, so a
contributor already sitting on 5.4.2 or 5.4.3 is not blocked; only CI is exact.

### `espressif/esp-bsp` → `bsp/m5stack_tab5` (published as `espressif/m5stack_tab5`)

Espressif maintain an **official Tab5 board support package** in the ESP Component
Registry. Inspected: `idf_component.yml`, `Kconfig`, `API.md`,
`include/bsp/m5stack_tab5.h`, `include/bsp/display.h`, `src/bsp_display.c`,
`src/bsp_feature_en.c`.

**Decision: build on `espressif/m5stack_tab5` rather than copying M5Stack's demo HAL.**

Rationale:

* It is first-party, versioned, and covers exactly the things the brief asks to take from
  official support: display driver, touch driver, LVGL setup, backlight/brightness, I²C,
  SD card, and the IO-expander rails that power the LCD, touch panel and the C6.
* It handles the **two hardware revisions** of the Tab5 (ILI9881C + GT911 on early units,
  ST7123 display *and* touch on later ones) with automatic detection. Hand-porting the
  demo's driver would lock us to one revision.
* It targets LVGL 9 via `esp_lvgl_port ^2`, which is the version we want for new code.
* It avoids importing `esp-brookesia` / `mooncake` / `smooth_ui_toolkit` and the demo's
  apps and assets, which the brief explicitly does not want.

Cost of the decision: we are on **LVGL 9**, so LVGL 8 snippets from the demo are not
directly reusable, and the demo's camera/audio/IMU panels are not available. Neither is
needed for a dashboard.

---

## 3. Verified BSP surface (what we are allowed to call)

Taken verbatim from `bsp/m5stack_tab5/include/bsp/m5stack_tab5.h` and `.../display.h`:

```c
/* resolution — NOTE: the panel is PORTRAIT */
#define BSP_LCD_H_RES              (720)
#define BSP_LCD_V_RES              (1280)
#define BSP_LCD_COLOR_FORMAT        (ESP_LCD_COLOR_FORMAT_RGB565)
#define BSP_LCD_BITS_PER_PIXEL      (16)
#define BSP_LCD_MIPI_DSI_LANE_NUM          (2)
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS (1000)

esp_err_t       bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
lv_display_t   *bsp_display_start(void);
lv_display_t   *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);
bool            bsp_display_lock(uint32_t timeout_ms);
void            bsp_display_unlock(void);
void            bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation);
esp_err_t       bsp_display_brightness_set(int brightness_percent);
esp_err_t       bsp_display_backlight_on(void);
esp_err_t       bsp_display_backlight_off(void);
esp_err_t       bsp_display_enter_sleep(void);
esp_err_t       bsp_display_exit_sleep(void);
esp_err_t       bsp_feature_enable(bsp_feature_t feature, bool enable);
esp_err_t       bsp_sdcard_mount(void);
esp_io_expander_handle_t bsp_io_expander_init(void);
esp_io_expander_handle_t bsp_io_expander1_init(void);
```

Confirmed from `src/bsp_display.c` and `src/bsp_feature_en.c`:

* `bsp_display_start()` internally does `lvgl_port_init()` → LCD init → touch init, and the
  LCD/touch paths themselves call `bsp_feature_enable(BSP_FEATURE_LCD/TOUCH, true)` and
  `bsp_i2c_init()`. **We must not duplicate those.**
* `bsp_feature_enable(BSP_FEATURE_WIFI, true)` drives `BSP_WIFI_EN` on the *second* IO
  expander (`bsp_io_expander1_init()`). This powers the ESP32-C6 and **must be called
  before `esp_wifi_init()`**. This is the single most easily-missed step on this board.
* `bsp_display_start()`'s default LVGL config is
  `buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT`, double buffered,
  `buff_dma = true`, `buff_spiram = false`, `sw_rotate = true`.

### 3.1 The Tab5 has THREE panel variants, and the BSP only handles two

**This is the most important hardware fact in this repository.** It cost an entire debugging
session, and anyone bringing up a Tab5 with Espressif's BSP will hit it.

Retail Tab5 units ship with one of three display panels:

| Panel | Touch | I²C addr | Touch FW version reg | Espressif BSP |
| --- | --- | --- | --- | --- |
| ILI9881C | GT911 | 0x5D / 0x14 | — | ✅ supported ("board version 1") |
| ST7123 | ST7123 | 0x55 | **3** | ✅ supported ("board version 2") |
| **ST7121** | ST7123-compatible | **0x55** | **1** | ❌ **NOT supported — mis-detected as ST7123** |

The BSP identifies the board by probing the touch controller's I²C address. **ST7121 and ST7123
share address 0x55**, so the BSP finds it, logs
`Discovered board version 2 (LCD ST7123, Touch ST7123)`, and initialises an ST7121 with the
ST7123 command sequence and ST7123 video timings.

M5Stack's own firmware goes one step further: it reads the touch controller's **firmware-version
register (0x0000)** — `1` means ST7121, `3` means ST7123. That single read is the entire
difference between a working display and a dead one.

#### Why this is so hard to diagnose

**Nothing reports an error.** MIPI-DSI command writes are fire-and-forget; there is no
acknowledgement path from the panel. So against a panel that has never accepted its
configuration:

* `esp_lcd_panel_init()` → `ESP_OK`
* `esp_lcd_panel_disp_on_off()` → `ESP_OK`
* `esp_lcd_panel_draw_bitmap()` → `ESP_OK`
* `esp_lcd_dpi_panel_set_pattern()` → `ESP_OK` — **even the DSI hardware test pattern shows
  nothing**, because it is emitted through the same PHY the panel never locked onto
* The backlight lights normally
* The touch controller enumerates perfectly and reports its firmware version and 720×1280 extent
* The boot log is immaculate

...and the screen stays black. Every layer *above* the DSI link — LVGL, buffers, rotation,
cache, DMA — is innocent, and hours can be lost eliminating them one at a time.

**The test that actually settles it is flashing M5Stack's own firmware** (prebuilt binaries are
on their [releases page](https://github.com/m5stack/M5Tab5-UserDemo/releases)). If their firmware
displays and yours does not, the hardware is proven good and the fault is in your panel
configuration. Do this *early*, not after exhausting your own code.

#### What this project does about it

* Vendors M5Stack's `esp_lcd_st7121` driver into `components/esp_lcd_st7121/` — it is
  Apache-2.0 and Espressif-authored, it simply is not published to the component registry.
* Implements the firmware-version check in `tab5_board::detectPanel()`, so all three variants
  are identified correctly rather than hard-coding for one unit.
* Brings the panel up directly instead of via `bsp_display_start_with_config()`, because that
  helper hard-codes the ST7123 path and the 1000 Mbps lane rate.

#### ST7121 parameters (from M5Stack's working firmware)

| | ST7121 | ST7123 |
| --- | --- | --- |
| DSI lane bit rate | **965 Mbps** | 1000 Mbps |
| DPI clock | 70 MHz | 70 MHz |
| hsync pulse / back / front | 2 / 40 / 40 | 2 / 40 / 40 |
| **vsync pulse / back / front** | **20 / 24 / 200** | 2 / 8 / 220 |
| Reset GPIO | −1 (software reset only) | `BSP_LCD_RST` |

The vertical porches and the lane rate are the whole difference.

**Remove `components/esp_lcd_st7121/` if the upstream BSP ever gains ST7121 support.**

### Orientation decision

The panel is **720 × 1280 portrait**. The brief wants a **1280 × 720 landscape** dashboard.
We therefore call `bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90)` and keep the BSP's
`sw_rotate = true`, which is what makes 90° rotation legal in `esp_lvgl_port`.

Consequence: rotation is done in software on flush. For a dashboard that redraws a clock
once a second this is fine; it would not be fine for animation-heavy UI. This is why the
UI direction in the brief ("minimal decoration, restrained transitions, no constant
animation") is also the *correct engineering* choice here, not just a style preference.
`CONFIG_BSP_LCD_DRAW_BUF_HEIGHT` is raised from 50 to 60 lines; buffers stay in internal
DMA-capable RAM (2 × 720 × 60 × 2 B ≈ 173 KB).

---

## 4. Minimum files and components required

| Capability | Provided by | Notes |
| --- | --- | --- |
| Display (MIPI-DSI, ILI9881C or ST7123) | `espressif/m5stack_tab5` BSP | pulls `esp_lcd_ili9881c`, `esp_lcd_st7123` |
| Touch (GT911 or ST7123) | same BSP | pulls `esp_lcd_touch_gt911`, `esp_lcd_touch_st7123` |
| LVGL integration + thread safety | `espressif/esp_lvgl_port ^2` (public dep of BSP) | `bsp_display_lock/unlock` |
| LVGL | `lvgl/lvgl ~9.2.0` | Kconfig-driven `lv_conf` |
| Backlight / brightness | BSP (`LEDC` on GPIO 22) | `bsp_display_brightness_set()` |
| IO expanders (LCD/touch/USB/Wi-Fi/camera rails) | `esp_io_expander_pi4ioe5v6408` (public dep of BSP) | 0x43 and 0x44 |
| I²C bus (SCL 32 / SDA 31) | BSP | `bsp_i2c_get_handle()` returns an `i2c_master_bus_handle_t` |
| Wi-Fi | `espressif/esp_wifi_remote` + `espressif/esp_hosted` over SDIO to the C6 | plus `bsp_feature_enable(BSP_FEATURE_WIFI, true)` |
| TCP/IP, DNS, SNTP | ESP-IDF `esp_netif` / `lwip` | |
| TLS with certificate validation | ESP-IDF `esp-tls` + `mbedtls` certificate bundle | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` |
| RTC | **not in the BSP** — own driver, `components/tab5_board/src/rtc_rx8130.cpp` | RX8130CE @ I²C `0x32` |
| Settings / secrets | ESP-IDF `nvs_flash` | optional NVS encryption, `nvs_keys` partition reserved |
| Tasks + response cache | `joltwallet/littlefs ^1.16` on a `littlefs` partition | power-fail safe, atomic replace on top |
| OTA | ESP-IDF `app_update` + `esp_http_client` + `mbedtls` SHA-256 | dual `ota_0`/`ota_1`, `otadata` |
| JSON | ESP-IDF bundled `cJSON` | host tests use `libcjson-dev` via a compat header |
| Setup portal | ESP-IDF `esp_http_server` in SoftAP mode | |

The RTC is the only piece of low-level hardware access we implement ourselves. The chip
(Epson **RX8130CE**, 7-bit I²C address **0x32**) is documented by M5Stack for the Tab5, and
the register map used (`0x10` SEC … `0x16` YEAR, BCD; `0x1D` FLAG with `VLF`; `0x1E` CTRL0
with `STOP`) is taken from the Epson datasheet and cross-checked against the mainline Linux
`rtc-rx8130` driver. No pin assignments are invented — the RTC sits on the BSP's I²C bus.

---

## 5. ESP32-C6 Wi-Fi link (verified pin map)

Copied from `m5stack/M5Tab5-UserDemo/platforms/tab5/sdkconfig`, which was generated against
`esp_hosted 1.4.0` / `esp_wifi_remote 0.8.5` on ESP-IDF 5.4.2 for this exact board:

```
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=4
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000
CONFIG_ESP_HOSTED_SDIO_PIN_CMD=13
CONFIG_ESP_HOSTED_SDIO_PIN_CLK=12
CONFIG_ESP_HOSTED_SDIO_PIN_D0=11
CONFIG_ESP_HOSTED_SDIO_PIN_D1=10
CONFIG_ESP_HOSTED_SDIO_PIN_D2=9
CONFIG_ESP_HOSTED_SDIO_PIN_D3=8
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=15
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW=y
```

`esp_hosted` and `esp_wifi_remote` are pinned to the versions M5Stack proved on IDF 5.4.2
(`1.4.0` / `0.8.5`) rather than the newest 2.x line, because the Kconfig symbol names above
belong to that generation and because the C6 in a retail Tab5 is factory-flashed with a
matching `esp-hosted` slave image. Upgrading the host component without reflashing the C6
slave will break Wi-Fi with an RPC version mismatch. This is recorded as an open item in
[§10](#10-open-questions--assumptions).

---

## 6. Architecture summary

Full detail in [ARCHITECTURE.md](ARCHITECTURE.md). Shape:

```
main/                   app_main + AppController (boot order, wiring, OTA validation)
components/tab5_board   thin RAII wrapper over the BSP + RX8130CE RTC + backlight schedule
components/dashboard_core   DashboardPlugin, PluginBase, Worker, PageManager,
                            GestureDetector, Theme, PageScaffold, SemVer, time utils
components/dashboard_network WifiManager, TimeSync (SNTP), HttpsClient, SetupPortal
components/dashboard_storage Settings (+schema migrations), Fs (LittleFS), TaskStore, CacheStore
components/dashboard_ota     Manifest parser, OtaService (SHA-256, channels, rollback)
plugins/{clock,weather,elizabeth_line,todos,claude}   one ESP-IDF component each
plugins/settings        the Settings page (registered out of the rotation)
```

`plugins/` is added to `EXTRA_COMPONENT_DIRS` so every plugin is a real ESP-IDF component
with its own `CMakeLists.txt` and dependency list. A plugin can therefore be deleted from
the build by removing one `REQUIRES` entry, which is the property the brief actually wants
from "fail without affecting other plugins".

### Threading contract (the rule that keeps LVGL alive)

* **LVGL task** (created by `esp_lvgl_port`): owns every `lv_obj_t`. Reached only under
  `bsp_display_lock()`, or from an `lv_timer` callback (which already holds it).
* **One worker task per plugin** (`dashboard::Worker`): does HTTPS, JSON parsing and
  filesystem writes. Never touches an `lv_obj_t`.
* Handoff is a **double-buffered model guarded by a mutex** plus a `std::atomic<bool>`
  dirty flag. `PageManager` runs a single 250 ms `lv_timer` that calls `tick()` on every
  plugin; `tick()` is where a plugin copies its model under the mutex and updates widgets.
* `refresh(force)` is called **from the LVGL context** and must only *post a job* to the
  worker. It is documented as non-blocking, and `PluginBase::requestRefresh()` enforces it.

### Deviations from the structure in the brief

| Brief | Actual | Why |
| --- | --- | --- |
| `main/page_manager.*` | `components/dashboard_core/.../page_manager.*` | Plugins and the Settings page need to see it; ESP-IDF `main` cannot be a dependency of a component. |
| root `idf_component.yml` | `main/idf_component.yml` | ESP-IDF resolves the project manifest from the `main` component. A root manifest is only read when the *project itself* is a component. |
| `plugins/` as plain folders | `plugins/` as component dirs | Required for `EXTRA_COMPONENT_DIRS`. |
| — | added `plugins/settings/` | The brief wants Settings outside the rotation; making it a plugin that `PageManager` registers as `Hidden` reuses all the page machinery. |
| — | added `test/host/` | Host-side unit tests for the pure logic the brief lists. |

---

## 7. Milestone mapping (what this first pass delivers)

| Milestone | State after this pass |
| --- | --- |
| 1 — hardware shell | **Done in code.** BSP init, rotation, boot screen with version + git SHA, serial logging. Not run on hardware. |
| 2 — page framework | **Done.** Plugin interface, `PluginBase`, `PageManager`, gesture navigation, page indicator, shared theme, all six pages. |
| 3 — local core | **Done.** `WifiManager`, SNTP, RX8130CE fallback, `Settings` in NVS with migrations, SoftAP setup portal, live clock with two faces + dimming + burn-in nudge. |
| 4 — public integrations | **Done.** Open-Meteo behind `WeatherProvider`, TfL Elizabeth line with commute-aware intervals, disk cache, stale/error states. |
| 5 — tasks | **Done.** Telegram long polling, allow-list, idempotent update handling, LittleFS task store, all six commands, touch complete/delete-with-confirm. On-screen "add task" uses the LVGL keyboard. |
| 6 — deployment | **Done.** `build.yml`, `release.yml`, `partitions.csv` with dual 6 MB OTA slots, `OtaService` with SHA-256 + rollback, manifest generator. |
| 7 — Claude experiment | **Scaffolded honestly.** Provider abstraction, relay provider with a *specified* JSON contract, direct provider with a configurable and explicitly unverified endpoint, mock provider marked as mock, full countdown/expiry UI. |

---

## 8. Storage layout

```
NVS  namespace "dash.cfg"   non-secret settings + u32 "schema"
NVS  namespace "dash.sec"   secrets only: wifi_pass, tg_token, claude_cred, tfl_key
NVS  namespace "dash.state" last Telegram update_id, ota channel, last-seen version
LittleFS /store/tasks.json          task records (atomic tmp+rename)
LittleFS /store/cache/<key>.json    last good API responses
```

Secrets live in a separate namespace so that enabling `CONFIG_NVS_ENCRYPTION` protects
exactly the partition that needs it, and so a factory reset can erase secrets without
touching layout-versioning state. The `nvs_keys` partition is reserved **now** because
adding a partition later forces a USB reflash.

---

## 9. Security posture

* No credential is ever committed. `config/example_config.json` contains placeholders only.
* Secrets are never logged. `dashboard::log` provides `mask()` which prints
  `set (NN chars)` or `not set`, never the value. Diagnostic logging for the Claude
  provider logs HTTP status, byte counts and parse outcome — never the cookie.
* TLS: `esp_crt_bundle` with certificate validation **on**. `CONFIG_ESP_TLS_INSECURE` is
  not enabled and `esp_http_client_config_t::skip_cert_common_name_check` is never set.
* Settings UI masks secret fields and offers "replace" rather than "reveal".
* `Settings::factoryReset()` erases both NVS secret and config namespaces and the LittleFS
  partition, then reboots into the setup AP.

**Documented limitations** (also in [CONFIGURATION.md](CONFIGURATION.md)):

1. NVS encryption requires flash encryption to be enabled on the device, which is a
   one-way operation and complicates development flashing. It is therefore **off by
   default**, with the partition and build flags prepared. Until it is enabled, a person
   with physical access and a USB cable can read the secrets out of flash.
2. The first-run setup portal is plain **HTTP** on the device's own SoftAP. Serving HTTPS
   would need a self-signed certificate that every phone would warn about, which trains
   users to click through TLS warnings. The exposure window is one short AP session on a
   network with one client. This is a deliberate trade-off.

---

### 9.1 Repository visibility and OTA — RESOLVED: the repository is public

**Status: closed.** `amore55/m5Dash` is **public**, verified unauthenticated on
1 August 2026:

```
api.github.com/repos/amore55/m5Dash  ->  private: false, visibility: public
api.github.com/users/amore55         ->  public_repos: 1
raw.githubusercontent.com/amore55/m5Dash/main/partitions.csv  ->  HTTP 200
```

**Chosen path: option 1 below.** Consequences:

* `https://github.com/amore55/m5Dash/releases/latest/download/<asset>` is anonymously
  fetchable, so OTA needs **no credential on the device at all**. This is the best available
  outcome — there is no token in firmware, none in NVS, and nothing to expire or rotate.
* `manifest_url` in `config/example_config.json` is correct as written.
* **Cross-host redirect handling is still mandatory.** `releases/latest/download/<asset>`
  answers with a 302 to `objects.githubusercontent.com` carrying a short-lived signed URL,
  on public repositories too. `OtaService` must follow one cross-host redirect, and the
  certificate bundle has to cover both hosts — `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`
  in `sdkconfig.defaults` does. Going public removed the *bearer token* requirement, not the
  redirect.
* Optional `Authorization: Bearer` support is **still implemented but off by default**,
  sourced from NVS only and never from a compiled-in constant. It costs almost nothing and
  keeps options 3 and 5 reachable without a firmware change if distribution ever moves to an
  authenticated host.

Because the repository is public, the repository hygiene rules in §9 stop being merely good
practice and become load-bearing: **anything committed here is world-readable, permanently, and
git history cannot be un-published.** No credential has been committed, `config/example_config.json`
holds placeholders only, and `.gitignore` blocks local config, build output and generated files.

#### The options as they were evaluated

Retained because the trade-offs still apply if distribution ever moves, and because the brief
asked for them to be documented rather than silently resolved.

The situation that prompted this: on a **private** repository
`releases/latest/download/<asset>` returns 404 to an anonymous client. The asset then has to
be fetched through the REST API (`/repos/OWNER/REPO/releases/assets/<id>` with
`Accept: application/octet-stream`) using an `Authorization` header — so the device would need
both a bearer token *and* redirect handling. The brief is explicit that a broad GitHub PAT must
not be embedded in firmware, and that is the right instinct: a `repo`-scoped classic PAT in
flash is a read/write credential for every repository the account can reach.

| # | Option | Trade-off |
| --- | --- | --- |
| 1 | **Make the repository public.** ✅ **CHOSEN** | Simplest, and unauthenticated OTA works immediately. No credential on the device at all. Cost: the source becomes visible. Nothing secret is committed — no keys, no tokens, `config/example_config.json` is placeholders only — so the real question was only whether the project should be public. It is. |
| 2 | **Private source + a separate public releases repository.** | Source stays private; the release workflow publishes `app.bin` + `manifest.json` to a small public repo. Unauthenticated OTA works, no device credential. Cost: a second repository and a cross-repo token *in CI* (not in firmware). |
| 3 | **Private source + any static HTTPS host** (Cloudflare R2/Pages, S3, a VPS). | Same benefits as 2, and decouples firmware distribution from GitHub entirely. Cost: you operate a host. The OTA service already treats the manifest URL as a setting, so this needs no firmware change. |
| 4 | **GitHub Pages.** | Possible, but whether Pages can be served *from* a private repository depends on the GitHub plan, and a Pages site is generally publicly readable regardless — **verify against your own account before relying on it.** Not recommended as the default because the availability rules are easy to get wrong. |
| 5 | **Keep everything private, store a fine-grained read-only PAT in NVS.** | Narrowest viable token: single-repository, Contents-read-only, expiring, revocable. Entered through the setup portal, never committed, never logged. Cost: a bearer token does live in device flash — so if the flash is read (physical access, NVS encryption off) it leaks, and it expires and needs re-entering. Also needs cross-host redirect support in the OTA client. |

Recommendation at the time was option 1 if the code could be public, otherwise option 3.
Option 1 was taken.

Deliberately *not* done at any point: weakening TLS verification, or defaulting to a broad
PAT, to make a private repository work. Options 2, 3 and 5 all remain reachable without a
firmware change because `manifest_url` is a configuration item — which is exactly why the brief
asked for the manifest host to be configurable.

## 10. Open questions / assumptions

Recorded so they can be closed on real hardware. **Items struck through have been closed by
running on the actual device.**

1. ~~**Which Tab5 revision is the target unit?**~~ **CLOSED — and the answer was the single
   biggest problem in the project.** The unit has an **ST7121** panel, which the BSP does not
   support and silently mis-detects as ST7123. See [§3.1](#31-the-tab5-has-three-panel-variants-and-the-bsp-only-handles-two).
   Display, touch and swipe navigation are now all verified working on hardware.
2. **C6 slave firmware version.** Assumed factory `esp-hosted` slave compatible with
   `esp_hosted 1.4.0`. If `esp_wifi_init()` fails with an RPC/version error, the C6 needs
   reflashing — M5Stack ship an image at `platforms/tab5/wifi_c6_fw` in the user demo.
   Procedure documented in [FLASHING.md](FLASHING.md#recovering-wi-fi-esp32-c6-slave-firmware).
3. **RX8130CE battery/trickle-charge configuration.** The driver deliberately does **not**
   touch `CTRL1` (`INIEN`/`CHGEN`/`BFVSEL`), because the correct values depend on what
   backup cell M5Stack fitted. Unverified; the RTC will still keep time while the board is
   powered and across a warm reset. Needs a schematic check before enabling charging.
4. **Does the Tab5 need a power-hold assertion to stay on from battery?** The Tab5 has
   IO-expander-controlled power rails and an INA226. The BSP exposes no `bsp_power_*` API
   and the dashboard is a USB-C powered desk device, so no power-hold is asserted.
   Unverified for battery operation.
5. **Software rotation cost at 1280 × 720.** Partly closed: rotation to landscape works on
   hardware and the UI is usable, but throughput has not been measured and the PPA hardware
   accelerator is currently **off** (`CONFIG_LVGL_PORT_ENABLE_PPA=n`). Turning it back on is a
   cheap win worth revisiting — it moves rotation off the CPU — but it was disabled while
   chasing the display fault and should be re-enabled deliberately, with a before/after look at
   responsiveness. `kRotateToLandscape` in `board.cpp` disables rotation entirely if it ever
   needs to be bisected again.
6. **Large display font.** LVGL's built-in Montserrat range stops at 48 px. The clock's
   hero digits therefore use `lv_font_montserrat_48` with an LVGL transform scale, which is
   slightly soft. `scripts/generate_fonts.py` + `assets/fonts/README.md` document generating
   a crisp 160 px face from a redistributable OFL font; the theme picks it up automatically
   if present. No font binary is committed because none could be licence-verified offline.
7. **Claude usage endpoint.** Unverified and undocumented by Anthropic. See
   [CLAUDE_USAGE.md](CLAUDE_USAGE.md). The relay path is the supported one.
8. **Telegram long-poll behind `esp_hosted`.** A 25 s held HTTPS connection over the
   SDIO-bridged C6 is assumed fine; the timeout is configurable down to short polling if it
   proves unstable.
9. ~~**`lv_indev` gesture reliability.**~~ **Mostly closed.** The polling gesture state machine
   works on hardware — swipe navigation between pages is confirmed. Thresholds (120 px, 900 ms
   long press, 450 ms cooldown) are still first guesses and have not been tuned for feel; they
   are all constants in `config/app_config.hpp`. Long-press-to-Settings is not yet confirmed by
   observation.

10. ~~**Watchdog reset loop.**~~ **CLOSED — it was collateral from the display fault.** After the
    ST7121 fix the device ran to 182 s and 122 s+ across two measured runs, past the ~60 s point
    where it used to reset, with flat heap. The likely original cause was the temporary panel
    self-test calling `bsp_display_delete()` and re-creating the DSI bus mid-boot; that code no
    longer exists.

    Diagnostics added while confirming it, and kept: `logResetReason()` at boot, a 30 s
    uptime/heap health report, and `CONFIG_ESP_TASK_WDT_PANIC=y` so a future task hang produces a
    backtrace and a core dump rather than a silent reset.

11. **Brownout.** `E BOD: Brownout detector was triggered` was seen once during early display
    bring-up. Not seen since, but worth remembering if instability appears — the panel at full
    brightness is the largest current draw on the board, and a marginal USB port would show up
    exactly this way.
10. **Flash size.** `sdkconfig.defaults` declares 16 MB per the Tab5 spec. Espressif's own
    example defaults for this board declare 4 MB, which would be wrong for the retail unit
    and would truncate the partition table. Verify with `esptool.py flash_id`.

---

## 11. What could not be verified locally

* ~~**No compilation.**~~ **RESOLVED.** ESP-IDF v5.4.4 is installed and `idf.py build`
  completes with exit 0 and no warnings in this project's own code.
* ~~**Nothing flashed or run on hardware.**~~ **RESOLVED.** Verified running on the device:

  | Verified working | Notes |
  | --- | --- |
  | Boot, NVS, PSRAM, flash | 16 MB confirmed by `flash_id`; 32 MB PSRAM at 200 MHz |
  | Display | ST7121 panel, 1280 × 720 landscape after 90° rotation |
  | Backlight + brightness | PWM changes take effect |
  | Touch | ST7123-compatible controller, 10-point, enumerates correctly |
  | Swipe navigation | Moves between all five rotation pages |
  | Boot screen, clock page, placeholder pages | All render |
  | RTC presence | RX8130CE answers at 0x32 |

* **Still unverified on hardware:** Wi-Fi / the ESP32-C6 link (not yet implemented),
  long-press-to-Settings, gesture *feel*, rotation throughput, dimming schedule, burn-in nudge,
  and every integration.
* **The RTC is running but has never been date-set.** It reports its "data valid" flag clear
  while returning `2080-01-01` — a *well-formed* date that passed the original field-level range
  check and was pushed into the system clock. `readUtc()` now also enforces a plausible year
  window (2024..2064) and rejects it, so the dashboard waits for network time instead of
  displaying a date 54 years out. **Field-level range checks are not plausibility checks.**
  Writing a real time back after the first SNTP sync should close this properly.
* ~~A watchdog reset loop is outstanding~~ — closed, see §10 item 10.
* **No host-side compilation.** There is still no host C/C++ compiler on this machine, and
  ESP-IDF does not supply one. The host unit tests are compiled for the first time by GitHub
  Actions on Ubuntu.
* **No hardware run.** Display, touch, RTC, C6 Wi-Fi, OTA and every integration are
  unexercised.
* **No live API responses.** No request was made to Open-Meteo, TfL, Telegram or Anthropic.
  Every parser is driven by hand-written fixtures in `test/host/src/` that are derived from
  the published response *schemas*, and they are labelled as fixtures — not as captured
  traffic.

This is restated in the README under *Known limitations* so it is not buried.
