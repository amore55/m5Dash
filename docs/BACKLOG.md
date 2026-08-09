# Backlog / resume point

**Local:** `c:\Moreno Functions\Projects\M5Dash` — where it belongs. A brief relocation to
`C:\dev\m5Dash` was made on the assumption that ESP-IDF cannot build through a path containing a
space; that was **tested and proved wrong on 5.4.4**, so it was reverted and the copy deleted.
See [IMPLEMENTATION_PLAN.md §1.1](IMPLEMENTATION_PLAN.md#11-spaces-in-the-project-path--tested-and-not-a-problem-on-544).
**Do not move this project again on that basis.**
**Remote:** `https://github.com/amore55/m5Dash` — **public**. Branch `main`.
**Toolchain:** ESP-IDF **v5.4.4** — activate with `. .\scripts\idf_env.ps1` (§1.1).

## ✅ THE BUILD IS GREEN

`idf.py build` exits **0**, with **zero warnings in our own code** (3 warnings total, all from
third-party managed components).

| | |
| --- | --- |
| Application | `build/tab5-desk-dashboard.bin` — 1,265,216 bytes (~1.24 MB) |
| Fits `ota_0` | 0x600000 (6 MB) — **80 % free**, ample headroom for the five real plugins |
| Bootloader | 21,120 bytes |
| Partition table | 3,072 bytes |
| OTA data | 8,192 bytes |
| Version stamped | `0.0.0-dev` + git SHA, from the CMake injection — working as designed |

This validates, for the first time, everything that was previously written blind: the Tab5 BSP
dependency set, every LVGL 9 call in the UI, the esp_hosted/esp_wifi_remote pins, the component
graph, and the partition table.

**Still unverified: anything requiring the actual device.** Nothing has been flashed or run.

**Resume at [§4.2](#42-componentsdashboard_storage)** — storage, then network, then OTA, then
the real plugins.

Read [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) first — it holds the researched
hardware facts and the design decisions. This file is only "where we stopped and what is
next".

---

## 1. Environment (verified — do not re-discover this)

ESP-IDF **has now been installed and proven to run**. Verified by execution, not assumed:

| Tool | State |
| --- | --- |
| ESP-IDF | ✅ **v5.4.4** at `C:\Espressif\frameworks\esp-idf-v5.4.4` |
| `IDF_TOOLS_PATH` | ✅ `C:\Espressif` (**not** the default `~/.espressif`) |
| RISC-V toolchain | ✅ `riscv32-esp-elf-gcc` 14.2.0 (crosstool-NG esp-14.2.0_20260121) |
| CMake | ✅ 3.30.2 |
| Ninja | ✅ 1.12.1 |
| IDF Python env | ✅ 3.11.2 at `C:\Espressif\python_env\idf5.4_py3.11_env` |
| esptool | ✅ v4.12.0 |
| esp32p4 target support | ✅ present |
| Git | ✅ 2.55.0 |
| **Host C/C++ compiler** | ❌ still absent — see §1.2 |
| Docker, WSL | ❌ absent (not needed) |
| `gh` CLI | ❌ absent (optional; only for cutting releases from the terminal) |
| System Python | 3.14.6 via `py` — **irrelevant**, IDF uses its own 3.11 |

**Still true: nothing in this repo has been compiled yet.** The toolchain now exists, but the
first `idf.py build` has not been run (blocked on §6a, the path move). The IDE's `#include`
errors are the absent IntelliSense config, not code faults.

### 1.1 Activating the environment — use `scripts/idf_env.ps1`

**ESP-IDF's own `export.ps1` is broken on this machine.** It invokes a bare `python`, which
Windows 11 resolves to the Microsoft Store app-execution-alias stub. The stub prints
`Python was not found...` and exits, and export.ps1 then dies with:

```
The expression after '.' in a pipeline element produced an object that was not valid.
At C:\Espressif\frameworks\esp-idf-v5.4.4\export.ps1:27 char:3
```

leaving `IDF_PATH` empty. **This looks like a broken install and is not.** `scripts/idf_env.ps1`
works around it by calling the virtualenv's `python.exe` by absolute path and applying
`idf_tools.py export --format key-value` itself.

```powershell
. .\scripts\idf_env.ps1            # MUST be dot-sourced
. .\scripts\idf_env.ps1 -Verify    # also prints the resolved tool versions
```

Verified working, exit code 0. The Start-menu **"ESP-IDF 5.4.4 PowerShell"** shortcut is an
equivalent alternative for interactive use.

One benign warning always appears and can be ignored (it is a missing diagnostic file in the
virtualenv, not a fault):
`WARNING: ... No such file or directory: '...idf5.4_py3.11_env\idf_version.txt'`

### 1.2 Host unit tests remain CI-only

There is **no host C/C++ compiler** on this machine, and the ESP-IDF install does not supply
one: `idf-git`'s `mingw64` directory carries Git's runtime libraries but no `gcc`, and while
`esp-clang` ships a `clang.exe` it has no Windows SDK / MSVC headers or import libraries to
link against.

So `test/host/` will be exercised by the GitHub Actions `host-tests` job on Ubuntu. To run it
locally, install MSYS2 (`winget install MSYS2.MSYS2`) plus `mingw-w64-x86_64-gcc` and
`mingw-w64-x86_64-cjson`. **Optional — do not treat as a blocker.**

---

## 2. Locked-in decisions (settled — do not reopen without a reason)

1. **ESP-IDF v5.4.4** — the 5.4 line is confirmed from `M5Tab5-UserDemo`'s README and
   `dependencies.lock` (they say 5.4.2), but 5.4.2 is not installable as a one-click unit on
   Windows, so the project standardises on 5.4.4. Reasoning in
   [IMPLEMENTATION_PLAN.md §2.1](IMPLEMENTATION_PLAN.md#21-why-544-and-not-542).
   CI container `espressif/idf:v5.4.4`; manifest `idf: ">=5.4,<5.5"`.
2. **Build on the official Espressif BSP `espressif/m5stack_tab5`**, not on a hand-port of
   M5Stack's demo HAL. It handles both Tab5 hardware revisions (ILI9881C+GT911 / ST7123)
   and gives us display, touch, LVGL, backlight, I²C and the IO-expander power rails.
   Consequence: **LVGL 9**, not the demo's LVGL 8.
3. Panel is **720 × 1280 portrait**; we run `bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90)`
   with the BSP's `sw_rotate = true` to get a **1280 × 720 landscape** UI.
4. **Wi-Fi** = `esp_wifi_remote` + `esp_hosted` over SDIO to the ESP32-C6, pinned to the
   versions M5Stack proved on IDF 5.4.2 (`esp_hosted 1.4.0`, `esp_wifi_remote 0.8.5`).
   SDIO pin map is in `sdkconfig.defaults` — **copied from M5Stack's committed sdkconfig, do
   not "tidy" it**.
5. `bsp_feature_enable(BSP_FEATURE_WIFI, true)` **must** run before `esp_wifi_init()`
   (powers the C6 via the second PI4IOE5V6408). Implemented as
   `tab5::Board::enableWifiRail()`.
6. **RTC** = Epson **RX8130CE @ I²C 0x32**, own driver (the BSP has none). Registers
   0x10–0x16 BCD, FLAG 0x1D (VLF bit 1), CTRL0 0x1E (STOP bit 6). `CTRL1` deliberately
   untouched.
7. **Tasks + API cache on LittleFS** (`joltwallet/littlefs`), not NVS, not SPIFFS —
   power-fail safety was an explicit requirement.
8. **`PageManager` lives in `dashboard_core`, not `main/`** (plugins and the Settings page
   need to see it; ESP-IDF `main` cannot be a component dependency). Deviation is recorded
   in the plan.
9. `plugins/` is on `EXTRA_COMPONENT_DIRS`; **each plugin is a real ESP-IDF component**.
10. **Threading contract** — this is the rule everything else hangs off:
    * `refresh(force)` is called on the **LVGL thread** and may only *post a job*.
    * All HTTPS / JSON / filesystem work happens on the plugin's **own worker task**.
    * Handoff is a mutex-guarded model + `std::atomic<bool>` dirty flag.
    * `tick()` runs on the **LVGL thread** at 250 ms and is the only place widgets change.
11. **Large fonts:** LVGL's built-in Montserrat stops at 48 px. Hero clock digits use
    montserrat_48 + LVGL transform scale; `scripts/generate_fonts.py` (still to write) will
    document producing a crisp 160 px face from an OFL font. **No font binary is committed.**

---

## 3. Done so far

```
docs/IMPLEMENTATION_PLAN.md          ✅ full plan, verified hardware facts, §10 open questions
docs/BACKLOG.md                      ✅ this file
CMakeLists.txt                       ✅ version/git-SHA injection, EXTRA_COMPONENT_DIRS
partitions.csv                       ✅ 16 MB, dual 6 MB OTA slots, otadata, nvs_keys, littlefs, coredump
sdkconfig.defaults                   ✅ P4/PSRAM/TLS-bundle/rollback/esp_hosted SDIO/LVGL/BSP
.gitignore                           ✅ blocks credentials, build output, generated fonts
.gitattributes                       ✅
.clang-format                        ✅
config/version.hpp                   ✅
config/app_config.hpp                ✅ all intervals, limits, gesture thresholds, TZ string
config/example_config.json           ✅ placeholders only
scripts/idf_env.ps1                  ✅ activation helper; works around the broken export.ps1
                                        (see §1.1). Tested, exits 0.

components/tab5_board/
  CMakeLists.txt                     ✅
  include/tab5_board/board.hpp       ✅ Board singleton + LvglLock RAII
  include/tab5_board/backlight.hpp   ✅ day/night schedule, midnight-wrap handled
  include/tab5_board/rtc_rx8130.hpp  ✅
  src/board.cpp                      ✅ BSP bring-up, rotation, RTC attach, Wi-Fi rail
  src/backlight.cpp                  ✅
  src/rtc_rx8130.cpp                 ✅ BCD, VLF validity, STOP-guarded write, own
                                        days_from_civil (no timegm dependency)

components/dashboard_core/          ✅ COMPLETE
  CMakeLists.txt                     ✅
  include/dashboard/json_compat.hpp  ✅ cJSON path shim for firmware vs host build
  include/dashboard/fixed_string.hpp ✅ FixedString<N>, no heap
  include/dashboard/semver.hpp/.cpp  ✅ strict parse, release > prerelease, isUpgrade()
  include/dashboard/time_utils.*     ✅ ESP-free. ISO-8601 parse, British date/time formatting,
                                        countdown, relative age, inTimeWindow (midnight wrap),
                                        own days_from_civil (no timegm/mktime dependency)
  include/dashboard/json_util.*      ✅ defensive cJSON accessors — every one tolerates a
                                        missing/null/wrong-typed field; RAII Doc using
                                        cJSON_ParseWithLength (HTTP bodies are not NUL-terminated)
  include/dashboard/plugin.hpp       ✅ DataState, DashboardPlugin, PageHost. Threading contract
                                        documented at the top — read this first
  include/dashboard/worker.*         ✅ one FreeRTOS task + bounded queue (depth 2, so refresh
                                        posts coalesce); heap-allocated std::function behind a
                                        pointer because FreeRTOS byte-copies queue items;
                                        synchronous stop()
  include/dashboard/theme.*          ✅ dark palette (hex literals live here and nowhere else),
                                        font accessors that degrade if a Kconfig font is absent,
                                        spacing scale, state->colour mapping, card/row/label/dot
                                        builders, applyHeroScale, dark default LVGL theme
  include/dashboard/page_scaffold.*  ✅ header (dot/title/clock/offline) + body + footer
  include/dashboard/plugin_base.*    ✅ THE important one. Implements the threading contract:
                                        refresh() only posts; fetch() on worker; updateUi() on
                                        LVGL via dirty flag; Loading/Ok/Stale/Error machine;
                                        auto-Stale on age; footer text; two separate mutexes so
                                        setError() inside a fetch() failure cannot deadlock
  include/dashboard/gesture_detector.* ✅ polls the indev on a 30 ms lv_timer instead of using
                                        LV_EVENT_GESTURE (which widgets swallow); swipe
                                        L/R/U/D + long press; integer dominance test;
                                        lv_indev_reset() so a swipe off a button doesn't click it
  include/dashboard/page_manager.*   ✅ pages built once and toggled hidden (leak-proof by
                                        construction); ONE lv_timer drives every plugin's tick()
                                        and refresh scheduling; staggered first refresh; overlay
                                        (Settings) with restore; order/enable; thread-safe
                                        setOnline(); lv_tick_elaps everywhere (49-day wrap)

plugins/clock/                      ✅ COMPLETE
  CMakeLists.txt                     ✅
  include/plugins/clock_plugin.hpp   ✅
  src/clock_plugin.cpp               ✅ two faces (minimal / split-flap cards), 24 h, British
                                        long date, configurable seconds, burn-in nudge,
                                        requiresNetwork()=false, only redraws when the displayed
                                        value actually changes
```

### Design changes made during implementation — do not re-litigate

1. **`tab5_board::Backlight` no longer owns the dim *schedule*.** The midnight-wrapping window
   predicate was needed for the TfL commute windows too, and `tab5_board` cannot depend on
   `dashboard_core` without a circular component dependency. So the single definition is
   `dashboard::timeutil::inTimeWindow()`, and `Backlight` now exposes
   `configure(day, night)` + `applyNightMode(bool)`. The board owns *how bright*; the app owns
   *when*. `inDimWindow()` and `scheduledPercent()` are gone.
2. **The page indicator is ONE widget on `lv_layer_top()`, not one per page.** `PageManager` only
   holds a `DashboardPlugin*` and cannot reach a plugin's footer. `PageScaffold::indicatorSlot()`
   and `footer_right_` were removed rather than widening the plugin interface.
3. **The header clock is rendered by `PluginBase`, not `PageManager`,** for the same reason.
   Plugins opt out with `showHeaderClock()` — the clock page does.
4. **`theme::applyHeroScale()` uses a percentage pivot** (`lv_pct(50)`), not half the measured
   width: it is normally called from `buildBody()` before layout has run, where
   `lv_obj_get_width()` returns 0 and the pivot would silently land on the corner.
5. **`cfg::kClockRefreshMs` is 60 s, not 1 h.** It only controls how often the clock re-checks
   whether the system time has become valid, so "waiting for time sync" clears promptly.
6. **A plugin with `refreshIntervalMs() == 0` is never scheduled at all** — including the
   one-off first refresh — so placeholder pages stay `Idle` instead of reporting
   "Updated just now".

---

## 4. Next actions, in order

### 4.1 `components/dashboard_core` — ✅ **DONE**

Left for reference. All nine sources listed in its `CMakeLists.txt` now exist.

`CMakeLists.txt` already lists all of these `SRCS`, so the component will not configure
until they exist.

| File | Contents |
| --- | --- |
| `include/dashboard/time_utils.hpp` + `src/time_utils.cpp` | **Must stay ESP-free** (host-tested). `setTimezone`, `localNow`, `localMinutesSinceMidnight`, `systemTimeValid`, `formatTime24h`, `formatBritishDate` ("Saturday 1 August 2026"), `parseIso8601Utc`, `formatRelativeAge`, `formatCountdown`, `parseHhMm`. |
| `include/dashboard/json_util.hpp` + `src/json_util.cpp` | Safe cJSON accessors: `getString/getInt/getDouble/getBool/getObject/getArrayItem` — every one tolerant of a missing or wrong-typed field. Plus `ParsedJson` RAII wrapper around `cJSON_ParseWithLength`. |
| `include/dashboard/plugin.hpp` | `DataState` enum {Idle, Loading, Ok, Stale, Error, Disabled}, `DashboardPlugin` interface (+ `refreshIntervalMs()`, `inRotation()`, `onNetworkStateChanged()`), and the `PageHost` interface the Settings page uses to close itself / trigger restart. |
| `include/dashboard/worker.hpp` + `src/worker.cpp` | One FreeRTOS task + bounded queue of `std::function<void()>`. Cap the queue (2) so posts coalesce and allocation is bounded. Watchdog-safe blocking. |
| `include/dashboard/theme.hpp` + `src/theme.cpp` | Dark palette (bg `#0B0D10`, surface `#14181D`, text `#E8EDF2`, secondary `#8A94A0`, accent `#4C9AFF`, ok `#3FB950`, warn `#D29922`, error `#F85149`), font accessors, spacing, `stateColour()`/`stateSymbol()`, `makeCard()`, `makeLabel()`. |
| `include/dashboard/page_scaffold.hpp` + `src/page_scaffold.cpp` | Standard page: root → header (title, status dot, clock) → body (flex-grow) → footer (last-updated left, page-indicator slot right). |
| `include/dashboard/plugin_base.hpp` + `src/plugin_base.cpp` | `PluginBase` implementing the threading contract: `refresh()` → `worker_.post` → `fetch(force)`; `tick()` → dirty flag → `updateUi()`. Subclasses override `onInitialise`, `buildBody`, `fetch`, `updateUi`, `onTick`. Owns state + last-success timestamp + error string. |
| `include/dashboard/gesture_detector.hpp` + `src/gesture_detector.cpp` | 30 ms `lv_timer` polling `lv_indev_get_state`/`lv_indev_get_point` — **not** `LV_EVENT_GESTURE`, because widget hit-testing swallows it. Emits SwipeLeft/SwipeRight/SwipeDown/LongPress. Uses the thresholds already defined in `config/app_config.hpp`. Calls `lv_indev_reset()` when a gesture fires so an in-progress widget press is cancelled. |
| `include/dashboard/page_manager.hpp` + `src/page_manager.cpp` | Registers plugins, creates **all** pages once up front (toggle `LV_OBJ_FLAG_HIDDEN`, so repeated navigation cannot leak), order/enable/default-page from settings, dot indicator, 140 ms fade transition, single 250 ms `lv_timer` driving every plugin's `tick()` + interval-based `refresh(false)`, Settings shown as an overlay that restores the previous page. Implements `PageHost`. |

### 4.2 `components/dashboard_storage`

`settings.hpp` (POD struct of `FixedString`s + a `schema` version), `settings_store.{hpp,cpp}`
(NVS namespaces `dash.cfg` / `dash.sec` / `dash.state`, secrets segregated, `factoryReset()`),
`settings_migrate.cpp` (**host-tested**), `fs.{hpp,cpp}` (LittleFS mount + atomic
`writeFileAtomic` via tmp+fsync+rename), `task.hpp` (id/title/status/priority/category/
created_at/due_at/completed_at/source), `task_store.{hpp,cpp}` (bounded to
`cfg::kMaxTasks`, atomic rewrite), `cache_store.{hpp,cpp}`.

### 4.3 `components/dashboard_network`

`wifi_manager` (STA + SoftAP, event-driven, exponential backoff, observable online/offline),
`time_sync` (SNTP → RX8130CE write on every success), `https_client` (esp_crt_bundle,
`cfg::kHttpMaxResponseBytes` ceiling, `kHttpTimeoutMs`, retry/backoff, **never logs headers
or tokens**), `provisioning` (`esp_http_server` on the setup AP + embedded HTML form).

### 4.4 `components/dashboard_ota`

`manifest.{hpp,cpp}` (**host-tested** parse of the manifest schema in the brief),
`ota_service.{hpp,cpp}`: raw `esp_ota_begin/write/end` (not `esp_https_ota`) so the image can
be SHA-256'd **while** streaming, verified, and only then have
`esp_ota_set_boot_partition()` called. Channel + `minimum_version` checks, size bounds from
`cfg::kOtaMin/MaxImageBytes`, progress exposed to the UI, manual by default.

### 4.5 Plugins (`plugins/*`)

Order to build: **clock → elizabeth_line → weather → todos → claude → settings.**
Clock first (no API dependency), Claude last (experimental, must not block the others).

* **clock** — two faces (minimal / split-flap), 24 h, `formatBritishDate`, seconds toggle,
  burn-in nudge using `cfg::kBurnInShiftPx`/`kBurnInPeriodMs`, no network need after sync.
* **elizabeth_line** — `https://api.tfl.gov.uk/Line/Elizabeth/Status`,
  `statusSeverityDescription` + disruption text, commute-aware 2 min / 10 min intervals,
  optional app key, cache, restrained warning state. Parser host-tested.
* **weather** — Open-Meteo behind a `WeatherProvider` interface, lat/lon from settings (no
  geocoding per refresh), °C + km/h, current/high/low/rain-probability/next hours, ~20 min,
  cache + stale display. Parser host-tested.
* **todos** — Telegram `getUpdates` long poll (25 s) on its own task; single allowed user id;
  **task id derived from `update_id`** so re-processing is idempotent; `last_update_id` in
  NVS; `/list /today /done /delete /clearcompleted /help`; touch-complete,
  delete-with-confirm, optional completed view, offline state, LVGL keyboard for local add.
  Command parser host-tested.
* **claude** — `ClaudeUsageProvider` + `DirectClaudeProvider` (configurable, **explicitly
  unverified** endpoint), `RelayClaudeProvider` (documented JSON contract),
  `MockClaudeProvider` (clearly labelled). 5 h / weekly utilisation, reset timestamps,
  locally-computed countdown, distinct auth-expiry vs network vs unsupported-response
  states, ~5 min refresh, **never logs the cookie**. Parser host-tested.
* **settings** — registered out of rotation; sections for every item in the brief's config
  list; secrets masked with "replace" not "reveal"; factory reset; OTA check/install; shows
  version + git SHA.

### 4.5a Wallpaper lock screen (requested 1 Aug 2026) — `plugins/wallpaper/`

A screensaver that takes over after a configurable idle period and needs a PIN to dismiss.

Registered **out of rotation**, exactly like Settings, so it reuses `PageScaffold`,
`PageManager::openOverlay()` and the page lifecycle rather than being a special case.

#### Behaviour

1. `PageManager` tracks `last_interaction_tick_`, reset by any touch press and by any
   programmatic navigation.
2. After `lock_idle_timeout_minutes` with no interaction, `openOverlay("wallpaper")` and set a
   `locked_` flag.
3. While locked, `GestureDetector::setEnabled(false)` — swipes must not be able to reach the
   dashboard behind the lock. The only live control is the wallpaper's own keypad.
4. Tapping the wallpaper reveals a numeric keypad. A correct PIN clears `locked_`, re-enables
   gestures and `closeOverlay()` restores the page that was showing.

#### Design decisions to honour

* **The wallpaper must not display page data.** Clock, date and (optionally) weather only. The
  point of a lock screen is defeated if the to-do list and Claude usage are readable over it.
  This is a functional requirement, not a styling preference.
* **Store a salted SHA-256 of the PIN, never the PIN.** mbedtls is already a dependency and
  `CONFIG_MBEDTLS_HARDWARE_SHA=y` is set. Salt goes in `dash.cfg`, hash in the **secret**
  namespace `dash.sec`. Never logged, masked in Settings, "replace" not "reveal" — same rule as
  every other secret.
* **FAIL OPEN on misconfiguration.** If locking is enabled but no PIN has been set, do **not**
  lock. Otherwise a half-finished settings change bricks the only interface the device has.
* **Rate-limit wrong attempts** with an increasing delay (e.g. 1 s, 2 s, 5 s, 15 s, capped) and
  no permanent lockout. This is a desk clock, not a vault.
* **A forgotten PIN must be recoverable.** The recovery path is a USB factory reset
  (`Settings::factoryReset()` equivalent over serial, or reflash). Must be documented in
  `README.md` next to the existing factory-reset instructions.
* **Be honest about what this is.** It is a deterrent against a passer-by reading your to-do
  list, not security: anyone with physical access and a USB cable can reflash the device. Say so
  in `docs/CONFIGURATION.md` rather than implying otherwise.

#### Interaction with the existing dim schedule

Two independent timers, and they must not fight:

* The **dim schedule** is time-of-day based (22:30–07:00) and already implemented in
  `tab5_board::Backlight`.
* The **idle lock** is inactivity based.

Locking should also drop the backlight to the night level via `Backlight::setTemporary()`, and
unlocking must restore the scheduled level by calling `applyNightMode()` again — not by assuming
the day level, or unlocking at 23:00 would blind the room.

#### New settings (added to `config/example_config.json`)

| Key | Notes |
| --- | --- |
| `lock.enabled` | bool, default **false** |
| `lock.idle_timeout_minutes` | 0 = never lock; default 15 |
| `lock.pin` | 4–8 digits. Placeholder only in the example file; stored hashed |
| `lock.show_weather` | bool, whether the wallpaper may show weather as well as the clock |
| `lock.wallpaper_style` | `clock` \| `clock_minimal` — room for an image later |

#### Open question

Whether the wallpaper should also be reachable on demand (a deliberate "lock now" action) as
well as on idle. A long press already opens Settings, so a second long-press gesture is not
available; the likely answer is a button on the Settings page.

### 4.6 `main/` — ✅ **DONE**

`idf_component.yml`, `CMakeLists.txt`, `placeholder_plugin.hpp` and `app_main.cpp` all exist and
build. The boot order implemented is: NVS (with erase-and-retry) → `Board::init()` → boot screen
showing version and git SHA → timezone → RTC restore → per-plugin `initialise()` (a failure is
logged and the plugin shown as disabled, never fatal) → `PageManager` begin/add/startPages →
delete the boot screen.

`app_controller.{hpp,cpp}` is still to come, once storage/network/OTA exist.

**Note on displayed strings — exact glyph range, read from the LVGL font sources:**

```
0x20-0x7F   printable ASCII
0xB0        °  degree sign
0x2022      •  bullet
+ the Font Awesome range behind LV_SYMBOL_*
```

Anything else renders as an empty box on the device. Em dashes, ellipses and middle dots had
crept into footer and boot-screen text and were replaced. **`°C` is available**, which matters
for the weather page. Comments and log messages are unaffected.

<details>
<summary>Original plan for this section (kept for reference)</summary>

This is now the highest-priority item, ahead of storage/network/OTA, because **it is what
unblocks the first `idf.py build`** — and that build is what validates the BSP dependency set,
the LVGL 9 API calls, the esp_hosted pins and the partition table all at once. Every one of
those is currently unverified.

Minimum for a first build (deliberately small — do NOT wait for the other components):

| File | Contents |
| --- | --- |
| `main/idf_component.yml` | The dependency manifest in §5 below. Include `esp_hosted` / `esp_wifi_remote` / `littlefs` even though no code uses them yet — the point is to surface version-solver problems now. |
| `main/CMakeLists.txt` | `SRCS "app_main.cpp"`, `INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/config"`, `REQUIRES tab5_board dashboard_core clock nvs_flash` |
| `main/placeholder_plugin.hpp` | A `PluginBase` subclass taking id/title/description, `refreshIntervalMs()` returning **0** so the scheduler skips it entirely. Temporary stand-in for weather / elizabeth / todos / claude / settings; delete each as the real plugin lands. Satisfies Milestone 2's "placeholder pages". |
| `main/app_main.cpp` | NVS init (with the erase-and-retry on `NO_FREE_PAGES`) → `Board::init()` → `timeutil::setTimezone(cfg::kDefaultTimezone)` → RTC `restoreSystemTime()` → under `LvglLock`: `PageManager::begin(board.display())`, plugin `initialise()`, `add()`, `startPages("clock")`. |

Then move the repo (§6a) and build (§6b).

`app_controller.{hpp,cpp}` comes **later**, once storage/network/OTA exist, and takes over the
full boot order: NVS → Board → boot screen → LittleFS → settings → timezone → PageManager +
plugins → Wi-Fi rail → Wi-Fi → SNTP → `esp_ota_mark_app_valid_cancel_rollback()` **only after**
start-up checks pass.

</details>

### 4.7 `scripts/` + `.github/workflows/`

`generate_manifest.py`, `calculate_sha256.py`, `generate_fonts.py`;
`build.yml` (PR + push to main; jobs: `host-tests`, `firmware`; pinned
`espressif/idf:v5.4.2`; caches `managed_components`; uploads app/bootloader/partition-table/
merged image/build metadata with commit SHA + version);
`release.yml` (tags `v*.*.*`; clean build; version from tag; SHA-256; `manifest.json`;
GitHub Release with notes).

### 4.8 `test/host/`

Tiny assert-based runner (`include/tiny_test.hpp`), `shim/esp_err.h` + `shim/esp_log.h`,
plain CMake, `libcjson-dev` on CI. Cover: semver, task command parsing, Claude/TfL/weather
parsing, countdown, config migrations, `inDimWindow`.

### 4.9 Docs — partially done

| File | State |
| --- | --- |
| `README.md` | ✅ Written. Honest status table, hardware/software requirements, quick start, layout, threading contract, known limitations, roadmap. Revisit as features land — several sections say "planned". |
| `docs/FLASHING.md` | ✅ Written, step by step, including no-driver explanation, download mode, the mandatory `flash_id` check, expected boot log and screen, troubleshooting, `erase-flash`, and C6 slave-firmware recovery. **Steps needing the device are marked ⚠️ UNVERIFIED — correct them once run for real.** |
| `LICENSE` | ✅ MIT. Copyright holder is `amore55`; replace with a legal name if preferred. |
| `LICENSES.md` | ✅ Written from the LICENSE files actually shipped in `managed_components/` after a build, not from memory. Includes the font glyph-range constraint. |
| `docs/ARCHITECTURE.md` | 🔲 To write |
| `docs/OTA.md` | 🔲 To write |
| `docs/CONFIGURATION.md` | 🔲 To write |
| `docs/CLAUDE_USAGE.md` | 🔲 To write |
| `assets/fonts/README.md`, `assets/icons/README.md` | 🔲 To write |

---

## 5. `main/idf_component.yml` — the dependency set to write

```yaml
dependencies:
  idf: ">=5.4,<5.5"
  espressif/m5stack_tab5: "^1.2.0"
  lvgl/lvgl: "~9.2.0"
  espressif/esp_hosted: "1.4.0"
  espressif/esp_wifi_remote: "0.8.5"
  joltwallet/littlefs: "^1.16.4"
```

`espressif/esp_lvgl_port`, `esp_io_expander_pi4ioe5v6408`, the LCD/touch drivers,
`esp_codec_dev`, `esp_video`, `bmi270` and `usb` all arrive transitively as **public**
dependencies of the BSP — do not list them again.

**Both version risks have now been resolved by an actual build:**

1. ✅ **RESOLVED — and it was real.** `lvgl/lvgl ~9.2.0` resolved to 9.2.2 and the build failed
   at object 1641 of 1768 with:

   ```
   managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:301:
     error: 'LV_COLOR_FORMAT_RGB565_SWAPPED' undeclared
   ```

   `esp_lvgl_port` 2.9.0 uses an LVGL **9.3** symbol but declares a loose enough constraint that
   the solver happily picked 9.2.2. **The floor has to be set in our manifest**, so it is now
   `lvgl/lvgl: "~9.3.0"` (resolves to 9.3.0). Do not lower it without also pinning
   `esp_lvgl_port` back to a release predating that symbol. Note this was *not* a solver error
   as predicted — it was a clean resolve followed by a compile failure, which is harder to
   diagnose from the manifest alone.

2. ⚠️ **Partially resolved.** `esp_hosted 1.4.0` / `esp_wifi_remote 0.8.5` resolve cleanly and
   compile against IDF 5.4.4 — CMake reports `Using Hosted Wi-Fi` and links esp_hosted with
   `--whole-archive`. **Runtime remains unverified:** the retail C6 must be running a compatible
   `esp-hosted` slave image. An RPC/version error from `esp_wifi_init()` means reflashing the C6
   (M5Stack ship an image at `platforms/tab5/wifi_c6_fw` in the user demo).

### Versions actually resolved by the first successful build

```
espressif/m5stack_tab5      1.2.0~1     espressif/esp_lvgl_port   2.9.0
lvgl/lvgl                   9.3.0       espressif/esp_hosted      (via manifest 1.4.0)
espressif/esp_wifi_remote   0.8.5       joltwallet/littlefs       1.22.3
espressif/esp_lcd_ili9881c  —           espressif/esp_lcd_st7123  1.0.2
espressif/esp_lcd_touch_gt911 —         espressif/esp_lcd_touch_st7123 1.0.2
idf                         5.4.4
```

Both display/touch driver pairs are present, so the BSP's runtime auto-detection of the two
Tab5 hardware revisions is compiled in.

### Other fix the build forced

`CONFIG_APP_ANTI_ROLLBACK` is a renamed symbol; ESP-IDF 5.4.4 warned and it is now
`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n`. Left off deliberately: efuse-based anti-rollback is
irreversible and would make development flashing a one-way door.

---

## 5a. ✅ CLOSED: repository visibility / OTA access

**The repository is public.** Verified unauthenticated on 1 August 2026:
`private: false`, `visibility: public`, `public_repos: 1`, and an anonymous fetch of
`raw.githubusercontent.com/amore55/m5Dash/main/partitions.csv` returned HTTP 200.

**No decision outstanding, and no credential needed on the device.**
`releases/latest/download/<asset>` is anonymously fetchable, so `manifest_url` in
`config/example_config.json` is correct as written. Full reasoning and the rejected
alternatives are in
[IMPLEMENTATION_PLAN.md §9.1](IMPLEMENTATION_PLAN.md#91-repository-visibility-and-ota--resolved-the-repository-is-public).

Two implementation requirements survive:

* **`OtaService` MUST follow one cross-host redirect.** `releases/latest/download/...` answers
  302 to `objects.githubusercontent.com` on public repos too. The full certificate bundle
  already covers both hosts. Going public removed the *token* requirement, not the redirect.
* Optional `Authorization: Bearer` support is still built, **off by default**, sourced from NVS
  only. Cheap, and keeps a move to an authenticated host possible without a firmware change.

⚠️ **Because the repo is public, `.gitignore` hygiene is now load-bearing rather than tidy.**
Anything committed is world-readable permanently and cannot be un-published. Run the staged-file
and token-shape checks in §7 before every push.

---

## 6. Open hardware questions still to close on real glass

Full list in [IMPLEMENTATION_PLAN.md §10](IMPLEMENTATION_PLAN.md#10-open-questions--assumptions).
The ones that will bite first:

1. Actual flash size — `sdkconfig.defaults` says 16 MB; Espressif's own example default for
   this board says 4 MB. Confirm with `esptool.py flash_id` before trusting the partition
   table.
2. Which Tab5 revision (ILI9881C+GT911 vs ST7123) — read it out of the boot log.
3. Whether software rotation at 1280 × 720 is fast enough.
4. RX8130CE backup-cell charging (`CTRL1`) — needs a schematic check before enabling.
5. Gesture thresholds are first guesses; all in `config/app_config.hpp`.

---

## 6a. Repo location — ✅ settled, stays at `c:\Moreno Functions\Projects\M5Dash`

Briefly moved to `C:\dev\m5Dash` on the assumption that ESP-IDF cannot build through a path
containing a space. **That assumption was tested and is false on 5.4.4**: a full build from the
`Moreno Functions` path exits 0 and produces the same binary. The move was reverted and
`C:\dev\m5Dash` deleted.

Findings worth keeping, so this is not re-litigated:

* **Spaces in the project path are fine on ESP-IDF 5.4.4.** Ninja quotes them correctly. The
  advice in older Espressif issue reports no longer applies. The ESP-IDF *installation* path is a
  separate question, and `C:\Espressif` sidesteps it anyway.
* **A directory junction does not work as a workaround** — `idf.py` resolves it back to the real
  target path. Irrelevant now, but do not reach for it if a path problem ever does appear.
* `Move-Item` will fail with a file-in-use error while VS Code has the folder open; `robocopy /E`
  works and leaves git history intact.

## 6b. Build and flash — ✅ build verified, flashing NOT yet attempted

```powershell
cd "c:\Moreno Functions\Projects\M5Dash"
. .\scripts\idf_env.ps1
idf.py set-target esp32p4     # first configure: downloads ~28 components, took ~2 min
idf.py build
```

Verified working: **exit 0, no warnings in our code.** See the build summary at the top of this
file for artefact sizes.

Then, with the Tab5 connected (USB-C, data cable; hold **Reset** ~2 s until the green LED
blinks rapidly to enter download mode):

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()          # find the port
esptool.py --chip esp32p4 -p COM7 flash_id            # CONFIRM FLASH SIZE FIRST — see §6 item 1
idf.py -p COM7 flash monitor                          # Ctrl-] to exit the monitor
```

⚠️ **Check `flash_id` before flashing.** `partitions.csv` and `sdkconfig.defaults` assume
**16 MB**. Espressif's own example default for this board declares 4 MB, and a 4 MB part would
make the partition table invalid. This is open question §6 item 1 below.

---

## 7. Housekeeping when resuming

* ✅ Committed and pushed as `ef000cb`; `origin` is configured and `main` tracks it. No further
  git setup is needed — subsequent work is ordinary `git add` / `commit` / `push`.
* ✅ The tree configures and builds cleanly. No outstanding structural blockers.
* ⚠️ **Nothing has been flashed or run on hardware.** The build proves the code compiles and
  links for esp32p4; it proves nothing about display output, touch, the RTC, the C6 Wi-Fi link,
  gesture thresholds or software-rotation performance. See §6 for the open hardware questions.
* The project lives at **`c:\Moreno Functions\Projects\M5Dash`** and stays there. See §6a.
* Before any commit, run `git status` and confirm none of these are staged: `sdkconfig`,
  `build/`, `managed_components/`, `dependencies.lock`, `config/local_config.json`, anything
  matching `*.bin` / `*.elf` / `*.map`. `.gitignore` already covers all of them — the check is
  belt and braces, because a leaked credential cannot be un-pushed.
* Exact first-commit / push / release commands still need writing into `README.md` (§4.9), and
  were also promised verbally as part of the final hand-off summary.

### Suggested order on resuming

1. ✅ ~~`dashboard_core`~~ — done.
2. ✅ ~~`main/`~~ — done.
3. ✅ ~~Move the repo and get a green build~~ — done.
4. **`components/dashboard_storage`** (§4.2) — settings and NVS come before everything that
   needs configuration, which is everything.
5. `components/dashboard_network` (§4.3), then `dashboard_ota` (§4.4).
6. Real plugins in the order in §4.5: elizabeth_line → weather → todos → claude → settings, then
   the wallpaper lock (§4.5a). Delete each `PlaceholderPlugin` from `app_main.cpp` as its real
   plugin lands.
7. CI workflows (§4.7), host tests (§4.8), docs (§4.9).

**Rebuild after each component**, now that a green baseline exists. A regression caught against
a known-good build is a five-minute fix; caught three components later it is an afternoon.

**Worth doing early, out of order:** flash the current firmware to the Tab5. It would close the
biggest remaining unknowns at once — panel revision, whether software rotation to 1280x720 is
fast enough, whether the gesture thresholds feel right, and the actual flash size (§6 item 1).
The clock page is real, not a placeholder, so there is something meaningful to look at.
