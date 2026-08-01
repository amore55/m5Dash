# Backlog / resume point

**Paused:** 1 August 2026, part-way through the first implementation pass.
**Local:** `c:\Moreno Functions\Projects\M5Dash` (must move — see §6a)
**Remote:** `https://github.com/amore55/m5Dash` — **public**. Branch `main`, in sync.
**Committed & pushed:** `ef000cb` "Add project scaffolding, Tab5 board support and implementation plan"
— 24 files, on top of GitHub's original `d9cc37d` *Initial commit*, whose `README.md` is
preserved and whose generic C/C++ `.gitignore` was merged rather than replaced.
**Toolchain:** ESP-IDF **v5.4.4** installed and verified working — activate with `. .\scripts\idf_env.ps1` (§1.1).

**Resume at [§4.1](#41-finish-componentsdashboard_core--resume-here).** Two environment
actions are outstanding first: the repo must move off a path containing a space
([§6a](#6a-blocker-before-the-first-build-move-the-repo-off-a-path-with-a-space)), and the
Tab5's real flash size must be confirmed
([§6b](#6b-first-build--the-exact-sequence-once-41-is-finished)).

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

components/dashboard_core/
  CMakeLists.txt                     ✅
  include/dashboard/json_compat.hpp  ✅ cJSON path shim for firmware vs host build
  include/dashboard/fixed_string.hpp ✅ FixedString<N>, no heap
  include/dashboard/semver.hpp       ✅
  src/semver.cpp                     ✅ strict parse, release > prerelease, isUpgrade()
```

Directory tree for everything else is already created (32 directories), just empty.

---

## 4. Next actions, in order

### 4.1 Finish `components/dashboard_core` ← **resume here**

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

### 4.6 `main/`

`idf_component.yml` (**the real dependency manifest** — see §5 below), `CMakeLists.txt`,
`app_main.cpp`, `app_controller.{hpp,cpp}`: boot order = NVS → Board::init → boot screen →
LittleFS → settings → timezone → PageManager + plugins → Wi-Fi rail → Wi-Fi → SNTP →
`esp_ota_mark_app_valid_cancel_rollback()` **only after** start-up checks pass.

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

### 4.9 Docs

`README.md` (full walkthrough per the brief), `ARCHITECTURE.md`, `FLASHING.md` (incl.
**recovering the ESP32-C6 slave firmware**), `OTA.md`, `CONFIGURATION.md`,
`CLAUDE_USAGE.md`, `LICENSES.md`, `LICENSE`, `assets/fonts/README.md`,
`assets/icons/README.md`.

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

⚠️ **Two version risks to check on the first CI run:**
1. `lvgl/lvgl ~9.2.0` vs whatever `esp_lvgl_port ^2` requires — a conflict here shows up as
   a component-manager solver error. Fix by widening to `^9.2`.
2. `esp_hosted 1.4.0` / `esp_wifi_remote 0.8.5` are the versions M5Stack proved, but the
   retail C6 must be running a compatible `esp-hosted` slave image. An RPC/version error
   from `esp_wifi_init()` means reflashing the C6 (M5Stack ship an image at
   `platforms/tab5/wifi_c6_fw` in the user demo).

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

## 6a. Blocker before the first build: move the repo off a path with a space

**ESP-IDF cannot build through a path containing a space** — not `IDF_PATH`, not the project
directory, not a component directory. The current location,
`c:\Moreno Functions\Projects\M5Dash`, has one in *"Moreno Functions"*. The failure is not a
clean error; it surfaces deep in the build as unescaped-path errors (typically around
`bootloader.elf`, or `Permission denied` from `idf_size.py`).

Nothing in the source has to change — it is a plain directory move.

```powershell
robocopy "c:\Moreno Functions\Projects\M5Dash" "C:\dev\m5Dash" /E /MOVE
# then reopen VS Code at C:\dev\m5Dash
```

Do it **before the first `idf.py build`**. Doing it mid-editing-session just invalidates
absolute paths in flight, which is why it has not been done yet.

## 6b. First build — the exact sequence, once §4.1 is finished

The component manager will resolve and download the BSP, LVGL, esp_hosted, esp_wifi_remote
and LittleFS on the first configure, so allow a few minutes and a working network.

```powershell
cd C:\dev\m5Dash
. .\scripts\idf_env.ps1
idf.py set-target esp32p4
idf.py build
```

**Do not expect this to pass first time.** The two known risks, both already noted in
[§5](#5-mainidf_componentyml--the-dependency-set-to-write):

1. `lvgl/lvgl ~9.2.0` vs whatever `esp_lvgl_port ^2` demands — appears as a component-manager
   solver error. Fix by widening to `^9.2`.
2. `esp_hosted 1.4.0` / `esp_wifi_remote 0.8.5` were resolved by M5Stack against IDF **5.4.2**;
   we are on **5.4.4**. Same minor line, so expected to be fine, but if the manifest solver
   objects, widen those pins.

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
* ⚠️ **`components/dashboard_core/CMakeLists.txt` lists nine source files, of which only
  `src/semver.cpp` exists.** CMake will fail to configure until §4.1 is finished. So either
  complete §4.1 before the first `idf.py build`, or temporarily trim that `SRCS` list. This is
  the single thing most likely to cause confusion on resuming.
* Before any commit, run `git status` and confirm none of these are staged: `sdkconfig`,
  `build/`, `managed_components/`, `dependencies.lock`, `config/local_config.json`, anything
  matching `*.bin` / `*.elf` / `*.map`. `.gitignore` already covers all of them — the check is
  belt and braces, because a leaked credential cannot be un-pushed.
* Exact first-commit / push / release commands still need writing into `README.md` (§4.9), and
  were also promised verbally as part of the final hand-off summary.

### Suggested order on resuming

1. Finish §4.1 (`dashboard_core`) — unblocks configure.
2. Write `main/idf_component.yml` + `main/` (§4.6) — minimum needed for a real build.
3. Move the repo (§6a), then `idf.py set-target esp32p4 && idf.py build` (§6b). **Get a green
   build before writing more code** — it validates the BSP dependency set, the LVGL 9 pin, the
   esp_hosted pins and the partition table all at once, and every one of those is currently
   unverified guesswork.
4. Then storage → network → OTA → plugins → workflows → tests → docs.
