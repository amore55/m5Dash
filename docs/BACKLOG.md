# Backlog / resume point

**Local:** `c:\Moreno Functions\Projects\M5Dash` — where it belongs. A brief relocation to
`C:\dev\m5Dash` was made on the assumption that ESP-IDF cannot build through a path containing a
space; that was **tested and proved wrong on 5.4.4**, so it was reverted and the copy deleted.
See [IMPLEMENTATION_PLAN.md §1.1](IMPLEMENTATION_PLAN.md#11-spaces-in-the-project-path--tested-and-not-a-problem-on-544).
**Do not move this project again on that basis.**
**Remote:** `https://github.com/amore55/m5Dash` — **public**. Branch `main`.
**Toolchain:** ESP-IDF **v5.4.4** — activate with `. .\scripts\idf_env.ps1` (§1.1).

---

## ⏭️ RESUME HERE — state at the end of the 14 August 2026 session

**Three real pages: the clock, the weather, and the Elizabeth line.** Stage 3 (local core), the
whole of `dashboard_network`, and the first two public integrations are done. The working tree is
clean and every commit was verified on hardware before it was made.

### What works right now, on the device

| | |
| --- | --- |
| Wi-Fi | Connects to `More2.4` on boot from stored credentials |
| First-run setup | Raises `DeskDashboard-Setup`, serves a form at `192.168.4.1`, takes credentials, connects |
| Time | SNTP syncs and writes back to the RX8130CE; clock shows real local time |
| **Weather** | **Live Open-Meteo forecast for the configured location, cached across reboots** |
| **Elizabeth line** | **Live status (worst-of), plus a 5-train board with per-train delay/cancellation, turning round at midday** |
| Settings site | **`http://deskdashboard.local`** — weather location, timezone, clock face, PIN, screen brightness. Always reach it by name, never by IP: the DHCP address has already moved twice (192.168.2.182 → 192.168.1.189) and any IP written down goes stale. If the name ever fails, that is an mDNS fault worth fixing, not a reason to look up the address. |
| Header | Always-visible signal icon, far right |

The settings site is **confirmed working from a browser** — the owner set a PIN through it and
changed the weather location to Greenhithe, both of which the device read back correctly. That
closes what was previously the second-biggest known gap.

### The "it doesn't boot, it stays black" fault — diagnosed, and it was not a boot fault

Worth reading before trusting any future report of a dead device, because the symptom was
convincing and the cause was ordinary.

The device was reported as not booting: black screen on power-on. It was in fact running perfectly
— answering HTTP on its IP *and* on `deskdashboard.local`, and printing health reports over serial
throughout. **The BSP logs every brightness change at INFO level**, and four saved boot captures all
showed the same two lines:

```
I (2955)  Setting LCD backlight: 70%     <- boots, screen on
I (33867) Setting LCD backlight: 12%     <- ~34 s later, night mode
```

It was 23:00, the dim window is 22:30-07:00, and 12 % on this panel reads as off. That is the whole
fault. Two things made it worse than a misunderstanding, and both are now fixed:

* **Nothing could wake it.** A dimmed screen and a dead one were indistinguishable.
* **Nothing could change it.** The settings page had no brightness or dim-window fields at all, so
  the only route to a brighter screen was a reflash.

**Diagnostic lesson worth keeping: `grep backlight` across the saved serial captures answered in one
step what reasoning about the display driver would not have.** The panel-detection failure in
README is a real and famous fault on this board, and it is the wrong answer here — a black screen is
not evidence of it when the firmware is demonstrably alive.

> ⚠️ **That last sentence cost a session.** It is true of *this* fault and it became a reason not to
> look again. A second, unrelated blank-screen fault followed and it **was** panel detection — see
> the next section. "The firmware is demonstrably alive" does not rule out a display bug, because
> nothing in this design makes the UI a precondition for the firmware running: Wi-Fi, HTTP and every
> plugin come up perfectly with the wrong panel driver loaded. Treat "alive but blank" as its own
> class of fault, not as evidence against the display.

**Verified: dimming follows the schedule, and a tap raises the panel to 70 % (observed twice).
NOT yet verified: the 60 s wake expiring back to night brightness.** Three attempts to observe it
failed for the reason in §1.1b — closing the monitor rebooted the device and wiped the wake. The
logic is four lines and reviewable, but it has not been seen working. Confirming it takes ten
seconds with the device in hand: tap it at night, wait a minute, watch it dim again.

Fixed by touch-to-wake (`Backlight::wake()`, driven from `GestureDetector::lastTouchTick()`),
brightness and dim-window controls on the settings page, and an asymmetric clamp: night brightness
may be 0, daytime brightness has a floor of 10 %. The asymmetry is the point — nothing would ever
raise the day level again, so 0 there is a soft brick, whereas 0 at night is now recoverable with a
finger. Note this also changed the dim tick from 30 s to 1 s, so at night the screen now dims about
five seconds after boot rather than half a minute.

### 🔴 The UI was living in internal SRAM, and nobody had measured it

**Fixed**, and it is the single biggest resource change this project has had.

`CONFIG_LV_USE_CLIB_MALLOC` sends every LVGL allocation through plain `malloc()`. That reads as
"PSRAM-backed, we have 32 MB", and it is wrong: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` is **16384**,
so any allocation *smaller* than 16 KB is served from internal SRAM first and only spills to PSRAM
once internal is exhausted. Every LVGL allocation is far under 16 KB, so the entire UI — every
widget of every resident page — sat in the one pool that is actually scarce on this board.

Measured at boot rather than reasoned about, which is the only reason it was found:

```
PAGEMEM: internal before 149271 after 101727 => 47544 B for 6 pages
```

| | Internal SRAM for pages | Steady free | Low-water |
|---|---|---|---|
| Before | 47,544 B (~7.9 KB/page) | 65 KB | **26.6 KB** |
| After  | **0 B** | 119 KB | **107.2 KB** |

`components/lvgl_heap` gives LVGL an allocator that tries PSRAM **first** and falls back to
internal — the inverse of the global policy, scoped to the one large consumer that cares about
neither latency nor DMA. Chosen over raising `ALWAYSINTERNAL` globally, which would also push the
Wi-Fi stack, the SDIO driver and every small driver buffer into PSRAM.

Two things worth keeping:

* **`WHOLE_ARCHIVE` is load-bearing, not caution.** lvgl's archive precedes ours on the link line,
  and a static library cannot resolve symbols the linker has already walked past. Without it the
  build fails with `undefined reference to lv_malloc_core` while the component compiles perfectly.
* **There is a regression canary** in `app_main`: `pages built: N B of internal SRAM`. It should
  print ~0. Anything else means the allocator stopped being linked or LVGL went back to CLIB malloc.

The owner also reported the UI became noticeably snappier, which was not the goal and is worth
noting: LVGL's draw buffers were already in PSRAM, so the win is presumably locality/fragmentation
rather than anything about the buffers.

### 🔴 The cold-boot dark screen — panel detection needed the LCD rail, and never had it

**Fixed.** Different fault from the one above, same symptom class, and it survived six weeks of
testing because **every test boot was a soft reset.**

Symptom: from the wall socket the backlight lights and the panel stays blank forever, while the
firmware behind it runs perfectly — Wi-Fi up, HTTP answering, plugins fetching, health reports
flowing. Flash or reset over USB and the screen works every single time.

Cause is an **ordering bug, and a circular one.** `Board::init()` identifies the panel by reading
the touch controller. On this board the touch controller has **no reset line of its own** —
`BSP_LCD_RST` is `GPIO_NUM_NC`, and `bsp_touch_new()` notes its reset is "usually shared with LCD
reset" — so with the LCD rail down it is held in reset and NACKs everything. We asserted only
`BSP_FEATURE_TOUCH` before probing and left `BSP_FEATURE_LCD` to panel creation, which runs
*afterwards*. So we needed the touch controller to choose the panel driver, and the touch controller
needed the rail that only the panel driver's own bring-up switched on.

Why a warm reset hid it completely: the IO expander carrying both rails is **a separate chip on I2C
with its own supply, and a CPU reset does not clear its outputs.** After any soft reset both rails
are still high from the previous run and the controller has been awake for as long as the device has
been plugged in, so it answers the first probe instantly.

Why it produced a blank screen instead of an error: a failed probe was not treated as a failure. It
was read as *evidence about which board this is* and fell through to ILI9881C — so an ST7121 was
driven by the wrong driver at the wrong DSI timings. The LCD rail, the DSI PHY and the backlight all
come up flawlessly; the panel simply never receives a signal it can lock to.

The captured proof, from one cold boot, is worth keeping because the two halves contradict:

```
W (2623) board: no ST712x touch controller after 774 ms; assuming ILI9881C
I (2623) board: detected panel: ILI9881C
I (2630) M5Stack Tab5: Install MIPI DSI LCD control panel   <- this brings up the LCD rail
I (3135) M5Stack Tab5: Discovered board version 2
I (3430) ST7123: Firmware version: 1(1.80.1.16)             <- version 1 == kFwVersionSt7121
```

We ruled the panel out at 2623 ms; the BSP read it perfectly at 3430 ms, and the version it read is
the one we had just excluded. **Waiting longer would never have fixed it** — the first attempt at
this raised the probe budget to 750 ms and still failed, because time was not the missing thing.

Fix, in `components/tab5_board/src/board.cpp`, both parts needed:

* assert **both** rails before probing, **LCD first** (both calls are idempotent, so it is free), and
* treat probe failure as "not ready yet" until a budget is spent, and only then as "not present" —
  50 ms settle, retry every 25 ms, plus retries on the version register.

After the fix, on a cold boot: `touch controller answered after 74 ms` → `detected panel: ST7121`.
That 74 ms is why the second part is not redundant — with the rail up the controller still needs
tens of milliseconds, so the old zero-delay probe would have stayed marginal.

**Diagnostic lesson: the decisive evidence was a serial capture that outlived the power cycle.** A
watcher that polls for the port, opens it, and reopens on disconnect (`scratchpad/coldboot.py`
pattern) is the only way to see a cold boot, because the port does not exist until the device
powers up. Every earlier attempt reset the device and captured the working case instead — see §1.1b.

Two loose ends deliberately left open:

* **Reset reason is `other watchdog (system/RTC)` (`esp_reset_reason=7`) on every boot, never
  `POWERON`.** Not a crash loop: the stored core dump was byte-identical (28256 bytes) across boots,
  so nothing new is being written. Most likely how this board's power-on presents. Unexplained.
* **A stale core dump sits at `0xf20000`** from an older build and is **unreadable** —
  `coredump SHA256(102446ad5) != app SHA256(908f3a758)`. `idf.py coredump-erase` would clear it so
  the next real crash is unambiguous; not done, as it is a device write nobody asked for.

### 🔴 Size response buffers from a DAYTIME sample

Two bugs in one session, same root cause: **every TfL response was measured at ~22:10, when the
timetable is at its thinnest.** Daytime is roughly double.

| Endpoint | Measured 22:10 | Measured 13:53 next day |
| --- | --- | --- |
| Liverpool St inbound arrivals | 15.0 KB / 17 predictions | **28.3 KB / 32 predictions** |
| Liverpool St ArrivalDepartures | 9.0 KB | **35.7 KB** |
| Abbey Wood outbound arrivals | 4.4 KB | 14.3 KB |

What that cost:

1. `TflProvider::kResponseBytes` was 24 KB. The device logged
   `response truncated at 24576 bytes`, and a truncated body is deliberately **not retried**, so
   the departure board was **empty every afternoon** while passing every test run at night.
   Now 48 KB.
2. `StatusTable::kMaxHints` filled **exactly** twice — 16 on a night response, then 32 on a
   daytime one. Raising the number was treating the symptom; the fix was to keep the **soonest**
   entries rather than the first ones, which the board's own parser already did. A full table now
   only drops trains later than the board shows.

**Rules that follow.** Measure an API's worst case at its busiest hour, not whenever you happen to
be working — a transport feed at 22:00 is not representative. And when a fixed-size collection
reports a count exactly equal to its cap, treat that as a truncation until proven otherwise;
a cap reached exactly is almost never a coincidence.

### Wanted on the Elizabeth line page — not started

**GitHub: token scope was the answer, and the status path is now verified.** The first token saw one
repository; a replacement sees five, three of which run Actions:

```
5 repositories from 25125 bytes (authenticated)
amore55/recipo: Passed (build-and-publish)
amore55/secondbrain: Passed (Build and publish Second Brain)
amore55/mediahub-pro: Passed (Build MediaHub Pro Image)
run lookups: 3 of 5 repositories have workflow runs, 0 failed
```

So if the list ever looks short, suspect the token before the firmware: a fine-grained token needs
read on Contents and Actions **and** the target repositories or organisations explicitly selected.

Timing confirms the estimate that shaped the page: 1 + 5 requests took ~10 s wall clock, so ~1.7 s
per serialised TLS request and ~19 s for a full ten-repo refresh. The progressive render is what
makes that tolerable.

**STILL UNVERIFIED on this page:** the Failure and Running states and their colours — every run
observed so far has passed — and the drill-down, which needs a tap. Also `github_all_repositories`
is stored but only reachable from the page's own filter buttons; there is no settings-page control
for it.

**A stored credential now forces a refetch.** Secrets bypass `Settings` entirely (they go straight
to `SecretStore`), so `write_settings` could not tell a plugin its token had been replaced, and
nothing refetched until the next scheduled refresh or a reboot — which is exactly the friction hit
when the second token was added. `WebServer::Callbacks::on_secrets_changed` closes it.

**National Rail departures through Abbey Wood.** Alongside the Elizabeth line board, show the next
few trains through Abbey Wood on the Southeastern side. **These are not in the TfL feed** — Abbey
Wood's mainline services are National Rail, so this needs a different upstream (Rail Data
Marketplace / the Darwin push-port feed, or an LDBWS-style departure-board web service), which means
its own credential, its own contract, and its own provider behind the same interface pattern.

Deliberately deferred. Worth scoping the data source before designing the page — the National Rail
options differ a lot in what they cost, whether they need a SOAP client the device can't reasonably
run, and whether a relay would be needed. Given the ~28 KB internal-SRAM headroom (§1.3), a fourth
HTTPS caller on this page is also worth measuring before committing to it.

### Session of 24 August 2026 — calendar, OTA, Claude retired for now

**Calendar (Today page): built, and the Entra ID app registration is still to be done.**
The owner has confirmed they can self-register the app but has not done it yet. When they do,
this is what goes on the settings page (`http://deskdashboard.local/settings` → "Calendar (Today
page)"):

1. Azure Portal → Microsoft Entra ID → App registrations → New registration.
2. **Authentication** → Advanced settings → **turn ON "Allow public client flows."** Device-code
   sign-in is a public-client flow and will not work without this.
3. **API permissions** → Microsoft Graph → Delegated → add **`Calendars.Read`** and
   **`offline_access`** → grant consent. NOT `Calendars.ReadBasic` — that scope specifically
   excludes `location`, which is the room this page exists to show.
4. Copy the **Application (client) ID** and the **tenant** (GUID or `*.onmicrosoft.com` domain)
   into the two settings fields.

Nothing else needed — device-code sign-in needs no client secret, and once both fields are saved
the Today page itself shows a code and a URL to finish sign-in on a phone. Until then it correctly
shows "add tenant + client ID" and does nothing (`ESP_ERR_INVALID_STATE`, verified on device, no
crash). **The entire OAuth exchange — device code request, token poll, calendarview fetch — has
never run against a real tenant** and is the thing to watch closely the first time it is tried.

**Claude usage page removed** (not migrated away, just unregistered — see app_main.cpp). No public
API exists for a personal Claude subscription's usage, so the page was a placeholder showing
nothing. `Settings::claude_provider` / `claude_organisation_id` / `claude_relay_url` are left in
NVS unused rather than cleaned up — reviving this later needs a plugin, not a settings migration.
**Telegram-based to-dos is the agreed next occupant of that page slot** — `dashboard_storage`'s
`TaskStore` was already built for exactly this and has never been wired to anything; that is the
remaining piece.

**OTA: built end-to-end, verified only up to the edge that needs a real release.** New
`components/dashboard_ota` (manifest parsing + `OtaService`), `HttpsClient::streamGet()` (chunked
download straight into `esp_ota_write()` while hashing, no size ceiling — see its own header for
why that is safe), three new endpoints (`/api/ota/status|check|install`), and a settings-page
"Firmware updates" section with live progress polling. Confirmed on device: clean boot, OTA
worker task starts, `confirmBootIfPending()` runs its no-op path correctly on this ordinary
(non-OTA) boot, no crash, 7 pages still built at 0 B internal SRAM.

**Measured cost: internal SRAM low-water dropped to 47.9 KB** (from the ~65 KB range in the
previous few sessions), because the OTA worker task's 16 KB stack — sized for the streaming
chunk buffer plus the SHA-256 context — is, like every plugin worker, allocated from internal
SRAM by `xTaskCreate`. Still comfortably above the ~14 KB level that previously caused a genuine
reboot loop, but worth re-measuring before adding anything else that starts its own task.

**NOTHING about the actual download-verify-apply path has been exercised.** There is no published
release, no real `manifest.json` anywhere, and therefore no live test of: the manifest fetch, the
GitHub-releases cross-host redirect (`objects.githubusercontent.com`, unauthenticated so the
credential-redirect-refusal rule does not apply to it — confirmed by reading the code, not by
trying it), the streamed SHA-256 verification, or `esp_ota_set_boot_partition` +
`esp_restart()`. Before trusting this against a real firmware image:

1. Publish a real `manifest.json` + `app.bin` (a GitHub Release is the design target) and confirm
   `sha256`/`size` in the manifest match the actual built binary exactly — a mismatch here fails
   *safely* (the device just refuses to apply it) but would be confusing to debug blind.
2. Press "Check for updates" and confirm the settings page shows `UpdateAvailable` with the right
   version.
3. Press "Install update" and watch the progress bar move, then confirm the device reboots into
   the new version and **stays up** — that last part is what actually exercises
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` and `confirmBootIfPending()`, which cannot be tested
   any other way.
4. Only then trust `ota_automatic_install`, which is off by default and calls the exact same
   `requestInstall()` path unattended every six hours (`kOtaCheckPeriodUs`) once turned on.

### Pick up here

**First, three small things left over from this session:**

1. **Look at the Elizabeth line page and say whether the layout works.** Its data path is verified
   from the serial log; the arrangement is arithmetic on paper. Same caveat as the weather page had.
   The board is now **five columns** — time, destination, platform, status, countdown — on a 1200 px
   body, so this is the page most likely to be cramped. The status column is the newest and is blank
   whenever TfL did not mention that train, which is expected, not a bug.
2. The weather details card was bumped one step up the type scale at the owner's request
   (`fontBody`/`fontTitle`). Confirm that reads well before treating it as settled.
3. **The new Screen section on the settings page has never been opened in a browser.** The
   round trip is implemented on both sides (`brightness`, `night_brightness`, `dim_start`,
   `dim_end`, and `min_brightness` reported by the device so the slider cannot disagree with the
   firmware's clamp), and it builds — but the two range inputs and the two time inputs have not
   been rendered. `style.css` has no rules for `input[type=range]` or `input[type=time]`, so they
   will appear in browser default styling until someone decides they care.

**Then: the overview / KPI page**, which is the owner's own long-standing want — one screen showing
the time, the line status without detail, the Claude percentage. It needs `summary()` added to
`DashboardPlugin`/`PluginBase` so each plugin can report a headline value and state; three plugins
now exist to summarise, which is what it was waiting for.

~~Then, in order: to-dos (Telegram) → Claude → OTA.~~ Done, in a different order — OTA and Claude's
retirement 24 August, to-dos 29 August. See that session's entry below.

**Before to-dos or Claude, read §1.3 and close known gap 2** — those are the first plugins to carry
a bearer token, and `Authorization` is not currently stripped across a cross-host redirect.

### Decisions taken this session (do not reopen without a reason)

* **Settings are configured in a browser, not by touch.** The device has no keyboard; typing a
  place name or a POSIX TZ rule on an on-screen keyboard is miserable. The on-device settings page
  remains the right home for toggles and brightness.
* **The web server never stops.** It answers on the setup AP and the home network alike.
* **PIN-gated**, reusing the lock-screen salted hash, verified per request. No PIN is set on the
  device today — the owner chooses one on first visit to `/settings`.
* **Geocoding runs in the browser, not on the device.** The page searches Open-Meteo from the
  phone or laptop viewing it and POSTs only the coordinates, which sidesteps needing an on-device
  geocoder entirely. Manual lat/long entry is the fallback.

### Known gaps, in rough priority order

1. **Internal SRAM headroom is thin — about 28 KB at the low-water mark with three pages.** See
   §1.3 for the full measured table. This is the constraint most likely to bite next, and it bites
   as a panic in an unrelated component rather than as an allocation failure where the memory was
   spent. Each further plugin costs ~8 KB of worker stack before it fetches anything.
2. **`Authorization` is not stripped across a redirect.** `esp_http_client` follows redirects and
   nothing removes the header if the redirect crosses hosts. Close this before the first
   credentialled API (Telegram, Claude) ships. Not urgent for Open-Meteo or TfL, neither of which
   uses a bearer token.
3. **No captive-portal DNS hijack**, so the setup page has to be typed rather than popping up.
4. **`components/dashboard_core/src/theme.cpp` and `web/style.css` duplicate the palette.** Change
   one, change the other.
5. ~~The `todos`, `claude` and `settings` pages are still `PlaceholderPlugin`~~ `todos` is now the
   real `tasks` page (Telegram-backed, see the 29 August session below); `claude` was retired;
   `settings` remains a placeholder by design (it is the overlay itself, not a page with content).
6. **The Elizabeth line page's LAYOUT has not been looked at by a human.** Its data path is verified
   from the serial log (status parsed worst-of, 4-5 departures, cache restored at boot); the
   arrangement of the status card and the board is arithmetic on paper. The weather page's layout
   HAS now been seen and approved.
7. **The board has only ever been seen in one direction.** It was built and tested in the evening,
   so only the after-noon Liverpool Street → Abbey Wood board has run against the live API. The
   morning direction (Abbey Wood, `direction=outbound`) was verified against the API from the
   development machine but has not been rendered on device. Worth a look one morning.
8. **The reason text is capped at three lines** with `LV_LABEL_LONG_DOT`, and at 512 characters in
   `LineStatus`. An observed disruption message ran to ~600 characters, most of it a list of
   operators accepting tickets. If the truncation loses something useful in practice, the fix is to
   strip the boilerplate tail rather than to grow the box — the departure board needs the space.
9. **No host tests exist yet** (§4.8). Both `weather_model` and `elizabeth_model` were written to be
   testable — no ESP-IDF, no LVGL, and the parsers take the current time as an argument precisely so
   they are deterministic — but there is no runner on this machine to test them with. Saved live
   fixtures would be worth capturing while the API is fresh in mind.

### Session of 29 August 2026 — Telegram to-dos, and §1.3's crash came back

**To-dos: built end-to-end, never exercised against a real bot token.** New `plugins/tasks/`:
`TelegramService` (long-polls `getUpdates`, one worker task, never returns) mutating the
already-built `dashboard_storage::TaskStore`, and `TasksPlugin` (the renamed `todos` page) purely
reconciling the UI against it — see `tasks_plugin.hpp`'s own header comment for why its `fetch()`
does no networking at all. Six commands: a bare message adds a task (`tg-<update_id>` makes
replaying an update idempotent), `/list`, `/done`/`/complete`, `/reopen`/`/undone`,
`/delete`/`/remove`, `/clear`. Only one Telegram user id is ever answered — everyone else's
messages are silently ignored, not even an error reply, so a stranger who finds the bot cannot
even confirm it exists. Settings page grew a "To-dos (Telegram)" section (bot token, write-only,
same pattern as every other secret; allowed user id, a plain field like `github_username`, not a
secret).

**One genuinely new credential-leak class, caught before shipping, not after:** Telegram's Bot API
puts the token in the URL PATH — `/bot<TOKEN>/getUpdates` — not a header (GitHub, Microsoft) or a
query parameter (TfL), which is what every prior integration's redaction assumed. The existing
`redactUrl()` keeps everything up to `?`, so it would have printed the token in full on every log
line. Fixed with `HttpRequest::path_is_sensitive` + `redactUrlHostOnly()` (keeps only the scheme
and host) in `dashboard_network`, used by both of `TelegramService`'s calls.

**§1.3 happened again, reproducibly, and the cause was NOT Telegram's own worker task.** First
boot with the new `tasks`/`telegram` workers added:

```
E (18392) dma_utils: esp_dma_capable_malloc(181): Not enough heap memory
assert failed: sdio_push_data_to_queue sdio_drv.c:701 (pkt_rxbuff)
```

Same failure family as §1.3's original table: a burst of sequential TLS handshakes at boot (every
plugin's first fetch, mDNS starting, SNTP starting, all within about ten seconds of "network
online") drove internal SRAM low enough that `esp_hosted`'s SDIO driver — an entirely unrelated
component — could not get a DMA-capable RX buffer and asserted. It reproduced with no Telegram bot
token configured, i.e. with `TelegramService`'s own worker doing nothing but the
"not configured, wait" branch — so the new *permanent* internal-SRAM cost of the feature was the
culprit, not anything it did at runtime.

Three fixes, in the order they were tried and measured on device (`heap_caps_get_minimum_free_size`
via the existing `health:` log line), each with a fresh cold boot after every change:

1. **`sendReply()`'s two ~4.6 KB percent-encoding buffers moved off the stack onto PSRAM**
   (`ResponseBuffer`, the same "big + short-lived + must not cost internal SRAM" tool §1.3 already
   established) — this was the single largest stack-resident thing the whole feature added.
   Crash still reproduced afterwards: this alone was not enough.
2. **`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`** (`sdkconfig.defaults`) — frees each TLS session's
   internal-only working buffers (the parts `MBEDTLS_EXTERNAL_MEM_ALLOC` does not cover) as soon
   as the handshake completes rather than holding them for the connection's life. One of the
   levers §1.3 already named "if headroom gets tight again." Reduced how often the crash
   reproduced but did not eliminate it outright over repeated cold boots.
3. **OTA's worker stack was 16 KB and never needed to be** — `ota_service.cpp` keeps its manifest
   and its streamed download entirely in PSRAM already (`ResponseBuffer`, `streamGet`'s sink); the
   only stack-resident locals are a `mbedtls_sha256_context` and a few short strings. Cut to the
   project's own 8192-byte default (`workerStackBytes()`'s convention). Telegram's own worker was
   also right-sized from an initial 16 KB guess down to that same 8192-byte default once its own
   big buffers were off the stack (fix 1).

**Measured result: internal low-water mark recovered to ~40–45 KB** across repeated cold boots —
healthier than the 47.9 KB figure from the *previous* session (OTA alone, before its stack was
right-sized), even with two more worker tasks and a much larger boot-time fetch burst (calendar,
weather, both Elizabeth line calls, and — this run — thirteen sequential GitHub API calls for two
account aliases, all serialised through `tlsGate()`). No crash across multiple resets after all
three fixes landed. **The lesson, worth restating because it cost the whole point of the session
to relearn:** a worker's stack size is a permanent tax on the same ~65 KB budget §1.3 measured,
regardless of what that worker actually does at runtime — size it from what the code really keeps
on the stack, not from copying a bigger sibling's number "to be safe." A generous guess here is not
conservative, it is a live regression waiting for the next plugin to trip over it.

**Still completely untested: the live bot.** No real Telegram bot token has been set on the
device. Before trusting this: create a bot via @BotFather, set the token and your own numeric user
id (from @userinfobot or similar) on the settings page, then work through all six commands from a
real chat and confirm `/list`'s short-id resolution, the full-vs-short id fallback in
`TaskStore::findIndexLocked`, and that a message from any OTHER Telegram account is truly ignored.

### A wanted feature, captured before it is forgotten

**An overview / KPI page**: one page showing the time, Elizabeth line status without detail, the
Claude percentage, and so on. It needs one small addition to the plugin interface — a `summary()`
virtual so each plugin can report its own headline value and state, which every plugin already
knows and nothing can currently ask for. Deliberately deferred until there are real plugins to
summarise, or four of five tiles would be blank.

---

## ✅ IT RUNS ON HARDWARE

The build is green (exit 0, no warnings in our code, ~1.65 MB against a 6 MB OTA slot) **and the
dashboard is running on the device.**

| Verified on hardware | |
| --- | --- |
| Boot, NVS, PSRAM, flash | 16 MB confirmed by `flash_id`; 32 MB PSRAM @ 200 MHz |
| **Display** | **ST7121 panel, 1280 × 720 landscape** |
| Backlight + brightness | PWM changes take effect |
| Touch | 10-point controller enumerates correctly |
| **Swipe navigation** | **Moves between all five rotation pages** |
| Boot screen / clock page / placeholders | All render |
| RTC presence | RX8130CE answers at I²C 0x32 |

### 🔴 The one thing that cost a whole session: **the panel is an ST7121**

Espressif's BSP supports ILI9881C and ST7123. It does **not** support the ST7121, and because
ST7121 and ST7123 share touch I²C address `0x55`, the BSP mis-detects it and initialises the
wrong panel. **Nothing errors** — every `esp_lcd_*` call returns `ESP_OK`, backlight lights,
touch works, log is immaculate, screen stays black.

Full write-up, including the parameters and how to detect it, in
[IMPLEMENTATION_PLAN.md §3.1](IMPLEMENTATION_PLAN.md#31-the-tab5-has-three-panel-variants-and-the-bsp-only-handles-two).

**Lesson worth keeping: when a display is dark but the log is clean, flash the vendor's own
firmware early.** That one test proved the hardware good and would have saved hours of
eliminating my own code.

### ✅ Watchdog reset loop — RESOLVED

It was collateral from the display fault, not an independent bug. Measured on the device after
the ST7121 fix: **uptime 182 s in one run and 122 s+ in another**, well past the ~60 s mark where
it used to die, with **flat heap** (12 bytes of drift over two minutes — noise, not a leak).

The most likely original cause was the old panel self-test calling `bsp_display_delete()` and
re-creating the DSI bus mid-boot; that code is gone.

Three diagnostics were added while confirming it, and all are worth keeping:

* `logResetReason()` at boot — turns a silent reboot into a labelled one, and shouts about
  panics, watchdogs and brownouts specifically.
* A 30 s health report (uptime / free heap / min-free-ever) — makes a reboot loop obvious within
  one report, and a slow leak obvious long before it becomes a crash.
* `CONFIG_ESP_TASK_WDT_PANIC=y` — a task-watchdog timeout now panics with a backtrace and writes
  a core dump instead of printing a warning. A task that stopped yielding is not recoverable, it
  is just failing quietly, so trading "recoverable" for "diagnosable" is the right call.

### ✅ Also fixed: RTC reported the year 2080 as valid

The RX8130CE on this unit runs but has never been date-set. Its registers decode to a
*well-formed* `2080-01-01`, which passed the original field-level range check and was pushed
straight into the system clock — so the dashboard would have confidently displayed a date 54
years out instead of admitting it did not know the time.

`readUtc()` now also requires the year to fall in **2024..2064**. Verified on hardware:

```
W rtc8130: RTC date implausible (expected 2024..2064); treating as unset.
          Raw 0x10..0x16 = 48 36 00 06 01 01 80 -> 2080-01-01 00:36:48
```

General lesson worth keeping: **field-level range checks are not plausibility checks.** Every
individual field here was legal.

**Resume at [§4.2](#42-componentsdashboard_storage)** — storage, network, OTA, real plugins.

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

### 1.1b Closing the serial monitor REBOOTS the device

Measured directly, and it invalidated three attempts at an on-device observation before it was
spotted. Two consecutive `miniterm` sessions five seconds apart:

```
first open : uptime 62 s
second open: uptime 32 s      <- went BACKWARDS
```

The ESP32-P4's native USB-serial-JTAG resets the chip when the host closes the port. Consequences
for any hardware verification done from a script:

* **Everything must happen inside ONE monitor session.** A test of the shape "do a thing, then
  close the monitor, wait, reopen and look" tests a freshly booted device, not the thing you did.
  This is how touch-to-wake's 60 s expiry evaded three separate captures.
* Every capture appears to start at a similar uptime — roughly the gap since the last capture
  closed. That is the tell.
* `run_in_background` a single long capture instead of chaining short ones.

Also, unrelated but in the same family: **redirect `python -u`**, not plain `python`. Python
block-buffers stdout to a file, and force-killing the process discards whatever has not flushed.
Boot captures look complete only because boot produces enough output to fill the buffer; a quiet
device logging one line every 30 s can lose the lot.

### 1.2 Host unit tests remain CI-only

There is **no host C/C++ compiler** on this machine, and the ESP-IDF install does not supply
one: `idf-git`'s `mingw64` directory carries Git's runtime libraries but no `gcc`, and while
`esp-clang` ships a `clang.exe` it has no Windows SDK / MSVC headers or import libraries to
link against.

So `test/host/` will be exercised by the GitHub Actions `host-tests` job on Ubuntu. To run it
locally, install MSYS2 (`winget install MSYS2.MSYS2`) plus `mingw-w64-x86_64-gcc` and
`mingw-w64-x86_64-cjson`. **Optional — do not treat as a blocker.**

### 1.3 🔴 Internal SRAM is the binding constraint, and TLS is what spends it

**Read this before adding anything that opens an HTTPS connection.**

The first HTTPS request the weather page made after boot put the device into a **reboot loop**:

```
E esp-aes: Failed to allocate memory for start alignment buffer
E esp-sha: Failed to allocate buf memory
E esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x0001
E dma_utils: esp_dma_capable_malloc(181): Not enough heap memory
assert failed: sdio_rx_get_buffer sdio_drv.c:670 (*buf)
```

Note where it actually died: **`esp_hosted`'s SDIO driver**, in an assert, several components away
from the code that spent the memory. The TLS handshake exhausted internal SRAM, and then the Wi-Fi
transport could not get a DMA-capable RX buffer and panicked. Chasing this from the assert alone
would lead into the wrong component entirely.

**Two things were wrong, both now fixed:**

1. `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC` was in force, so the 16 KB TLS input buffer, the output
   buffer and the whole certificate-chain parse came out of internal SRAM. `sdkconfig.defaults`
   claimed in a comment that "TLS buffers live in PSRAM" — it was never actually configured.
   **Now `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`.**
2. The health report printed only total free heap. That is ~31 MB of PSRAM and it **stayed at 31 MB
   right up to the panic**, so the log actively hid the problem. It now prints internal and
   DMA-capable free separately, plus the internal low-water mark.

**Then the Elizabeth line page arrived and the answer to "what if two plugins handshake at once"
turned out to be: the low-water mark halves.** The full measured sequence, all on device, all with
`heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`:

| Change | Internal free | **Internal low-water** |
| --- | --- | --- |
| mbedTLS in internal SRAM, weather only | — | **reboot loop** |
| `MBEDTLS_EXTERNAL_MEM_ALLOC`, weather only | 78 KB | 40.5 KB |
| \+ Elizabeth line, both workers fetching freely | 60 KB | **14.1 KB** |
| \+ TLS serialised device-wide (`tlsGate`) | 63 KB | 22.0 KB |
| \+ cJSON allocating from PSRAM | 61 KB | 23.4 KB |
| \+ `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | 65 KB | **28.2 KB** |
| \+ the Elizabeth line's 3rd request (per-train status) | 63 KB | **24.7 KB** |

That last row is the one to watch: **adding a single extra HTTPS call to an existing page cost
~3.5 KB of the low-water mark**, even with TLS serialised. Budget roughly that much per new upstream
when planning a page, and re-measure rather than assuming the serialisation makes calls free.

Two of those are structural and are the ones that matter:

* **Response buffers come from PSRAM** (`dashboard/net/response_buffer.hpp`). A `char buf[24576]`
  member on a statically allocated plugin is 24 KB of internal SRAM for the life of the device.
  This is why `WeatherProvider::fetch()` takes a buffer instead of owning one.
* **One TLS session at a time, device-wide** (`tlsGate()` in `https_client.cpp`). This is what makes
  the peak cost independent of how many plugins exist — the sixth integration cannot reintroduce
  the crash. It is the single most valuable change here: +8 KB on its own.

**A prediction that was wrong, recorded because it cost time:** cJSON's node allocations were
expected to be the main remaining consumer. Moving them to PSRAM gained 1.4 KB, not the ~10 KB
expected. The bulk of the transient demand is the TLS session itself. Do not re-derive this.

**Still worth knowing:** each plugin's worker task adds ~8 KB of internal stack permanently, so
todos and Claude will cost ~16 KB of the 65 KB baseline between them. If headroom gets tight again:

* `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` is 16384, the TLS maximum record size. No API here sends
  16 KB records; 4096 would do. (With EXTERNAL_MEM_ALLOC those buffers are already in PSRAM, so
  the gain may be small — measure before believing it.)
* `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` frees handshake buffers once a session is established.
* Reduce `workerStackBytes()` per plugin — the default 8192 is sized for a TLS handshake plus a
  parse, and a plugin doing neither can be far smaller (the clock already uses 3072).

Related, found at the same time: esp_http_client's default 512-byte request buffer is **too small
for these APIs** and logged `E HTTP_HEADER: Buffer length is small to fit all the headers` on every
fetch. Open-Meteo's forecast URL is ~500 characters because it names every variable it wants.
`HttpsClient` now sets `buffer_size_tx = 1024`.

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

components/esp_lcd_st7121/           ✅ VENDORED (Apache-2.0, Espressif-authored)
  CMakeLists.txt                     ✅ carries the full explanation of why it exists
  esp_lcd_st7121.c                   ✅ from m5stack/M5Tab5-UserDemo, byte-for-byte
  include/esp_lcd_st7121.h           ✅ delete this component if the BSP ever adds ST7121

plugins/clock/                      ✅ COMPLETE
  CMakeLists.txt                     ✅
  include/plugins/clock_plugin.hpp   ✅
  src/clock_plugin.cpp               ✅ two faces (minimal / split-flap cards), 24 h, British
                                        long date, configurable seconds, burn-in nudge,
                                        requiresNetwork()=false, only redraws when the displayed
                                        value actually changes
```

### Display bring-up decisions — do not undo these

7. **`tab5_board` does the panel bring-up itself.** It does NOT call
   `bsp_display_start_with_config()`, because that helper hard-codes the ST7123 path and a
   1000 Mbps DSI lane rate. `Board::init()` instead: detects the panel → creates the DSI bus,
   panel IO and panel → `lvgl_port_add_disp_dsi()` → `bsp_touch_new()` + `lvgl_port_add_touch()`.
   This mirrors `bsp_display.c:327-385`; **diff against it if the BSP is ever updated.**
8. **`detectPanel()` reads the touch firmware-version register**, not just the I²C address.
   That is the only way to tell ST7121 from ST7123. Do not "simplify" it back to a probe.
9. **ST7121 uses 965 Mbps and vsync 20/24/200.** ST7123 uses 1000 Mbps and 2/8/220. Both paths
   are kept so the project works on any Tab5, not just this one.
10. **`CONFIG_LVGL_PORT_ENABLE_PPA=n` and the cache/XIP options are currently off.** They were
    disabled while chasing the display fault and are *not* known to be harmful — PPA in
    particular is a real win (hardware rotation). Re-enable them **one at a time**, testing the
    display after each, now that there is a known-good baseline.
11. **The self-test scaffolding is kept but disabled.** `kBootSelfTest` in `app_main.cpp` and
    `kRotateToLandscape` in `board.cpp` are bisect switches that earned their place. Leave them.

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

### 4.2 `components/dashboard_storage` — ✅ **DONE and verified on hardware**

| File | What it does |
| --- | --- |
| `settings.hpp` / `.cpp` | Every user-configurable value, no heap, **no secrets**. `clampToValidRanges()` so corrupt NVS cannot produce an impossible UI state. |
| `settings_migrate.*` | Versioned schema with a migration chain. Newer-than-known schema (OTA rollback) keeps values rather than resetting them. Host-testable. |
| `settings_store.*` | NVS, **one key per field** — adding a field needs no migration, and one bad entry loses one setting rather than the lot. Persists defaults on first boot so the schema gets stamped. |
| `secret_store.*` | Separate NVS namespace. Values never logged (`describe()` reports presence and length only), `ScopedSecret` zeroes on scope exit, lock PIN stored as salted SHA-256 with constant-time compare. |
| `fs.*` | LittleFS mount (formats if unmountable), `writeAtomic()` = temp + fsync + rename, refuses to write below 10 % free. |
| `task_store.*` | Bounded to `kMaxTasks`, **upsert by id** so replaying a Telegram `update_id` is idempotent, atomic whole-file rewrite, mutex-guarded. A corrupt file starts empty and is left on disk for recovery. |
| `cache_store.*` | One file per key under `/store/cache/`, key validation as a path-traversal guard, exposes entry age so the UI can say "data from 3 hours ago". |

**Verified on device:** partition formats on first use, mounts cleanly thereafter (3072 KB, 99 % free),
settings round-trip across reboots with the schema stamped, task store starts empty and loads.

Wired into `app_main`: filesystem mount is non-fatal, settings drive timezone / brightness /
clock face / page order / enabled pages, and the dim schedule is evaluated on the 30 s timer.

<details>
<summary>Original plan for this section</summary>

`settings.hpp` (POD struct of `FixedString`s + a `schema` version), `settings_store.{hpp,cpp}`
(NVS namespaces `dash.cfg` / `dash.sec` / `dash.state`, secrets segregated, `factoryReset()`),
`settings_migrate.cpp` (**host-tested**), `fs.{hpp,cpp}` (LittleFS mount + atomic
`writeFileAtomic` via tmp+fsync+rename), `task.hpp` (id/title/status/priority/category/
created_at/due_at/completed_at/source), `task_store.{hpp,cpp}` (bounded to
`cfg::kMaxTasks`, atomic rewrite), `cache_store.{hpp,cpp}`.

</details>

### 4.3 `components/dashboard_network` — ✅ **DONE and verified on hardware**

| File | What it does |
| --- | --- |
| `wifi_manager.*` | STA + SoftAP, event-driven, exponential backoff, observable online/offline. `scan()` returns a flat `ScanResult` array, strongest first, duplicate SSIDs collapsed. AP and station state are **independent** (`ap_active_`), so the station can associate while the portal is still served. `WifiFailure` classifies disconnect reasons into "retrying could help" vs not. |
| `time_sync.*` | SNTP, started only once the link is up. Each success is polled off the lwip task by the supervisor and written back to the RX8130CE, which is what stops the RTC drifting. |
| `https_client.*` | The single outbound path. Cert-bundle validation with no opt-out, caller-supplied response ceiling, timeout, bounded retries. Logs scheme+host+path only — **never the query string**, because TfL puts its app key there. |
| `web_server.*` + `web/` | Configuration site on port 80, always running, answering on both the setup AP and the home network. `/` Wi-Fi, `/settings` weather location + timezone + clock. PIN-gated via `X-Dash-Pin` against the lock-screen hash. Pages are real files under `web/`, embedded with `EMBED_TXTFILES`. |

**Verified on device:** first-run portal takes credentials and connects; SNTP syncs and the RTC
write-back lands; the settings API round-trips coordinates and preserves fields absent from the
form; the full PIN lifecycle behaves (open with none, 401 without/wrong, 200 with, removal itself
requiring it); HTTPS reaches api.open-meteo.com over a real TLS handshake, refuses `http://`, and
reports truncation rather than handing back half a JSON document.

Wired into `app_main`: a Wi-Fi supervisor task owns the decision to raise or close the setup
portal (3 auth rejections → portal; 20 min unreachable → portal; retry every 5 min while it is
up), mDNS advertises `deskdashboard.local`, and the header carries an always-visible signal icon.

**Not done here:** no captive-portal DNS hijack, so the setup page must be typed rather than
popping up. `esp_http_client` follows redirects but nothing yet strips `Authorization` on a
cross-host redirect — worth closing before any credentialled API ships.

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
* **elizabeth_line** — ✅ **DONE.** Status plus a live departure board, commute-aware 2 min / 10 min
  intervals, optional app key, cached status. Parser host-*testable*, not yet host-*tested*.

  **The board turns round at midday**: before noon Abbey Wood → Liverpool Street, after noon
  Liverpool Street → Abbey Wood, five departures either way. That is deliberately NOT tied to the
  commute windows in Settings (those control refresh cadence only) — tying it to them would leave
  the board blank-headed at 06:30 or 21:00, when which way you are going is still obvious.

  | File | Contents |
  | --- | --- |
  | `include/plugins/elizabeth_model.hpp` + `src/elizabeth_model.cpp` | **ESP-free and LVGL-free.** `ServiceLevel`, `LineStatus`, `Departure`, `BoardData`, `parseLineStatus`, `parseArrivals`, plus `normalisePlatform`/`shortenDestination`. The header carries the five API findings below — read it before touching the filtering. |
  | `include/plugins/tfl_provider.hpp` + `src/tfl_provider.cpp` | Naptan constants, `Journey`, and the two calls. Owns the direction/destination filtering so the page cannot get it subtly wrong. |
  | `include/plugins/elizabeth_plugin.hpp` + `src/elizabeth_plugin.cpp` | Status card over departure board. Countdown re-renders every 250 ms tick from `timeToStation` aged by elapsed time, so it ticks down between refreshes. |

  **Five things the live API does that the documentation does not prepare you for** — every one
  established by querying it, and every one capable of producing a plausible, wrong board:

  1. **A line can carry more than one `lineStatus` at once.** Observed: severity 9 "Minor Delays"
     AND severity 6 "Severe Delays" simultaneously, for different sections. Taking the first would
     have under-reported. The parser keeps the worst — and **lower severity is worse** in TfL's
     scheme (10 = Good Service, 6 = Severe Delays, 2 = Suspended).
  2. **`direction` inside a prediction is frequently an empty string**, so it cannot be used to
     filter client-side. The `direction=` query parameter works and is applied server-side — and
     halves the response, 31 KB → 15 KB at Liverpool Street.
  3. **Liverpool Street has two stop points on this line.** `910GLIVSTLL` is the low-level core
     platforms, where Abbey Wood trains run; `910GLIVST` is the mainline surface station. Use LL.
  4. **A departure board must exclude trains terminating where you stand.** Abbey Wood's arrivals
     include trains whose destination *is* Abbey Wood. Filter on `destinationNaptanId`, not on the
     name — names come in inconsistent forms ("Paddington", but "Maidenhead Rail Station").
  5. **Leaving Abbey Wood there is no single "towards Liverpool Street" destination**: trains show
     as Paddington, Maidenhead, Heathrow, Reading or Hayes & Harlington. They all call at Liverpool
     Street, because Abbey Wood is a terminus with one way out — so that board's rule is "everything
     outbound", not "everything to Liverpool Street".

  The feed is also **not sorted by time**, so the parser insertion-sorts into the five-slot array.

  **Per-train status (added after the first build).** `StopPoint/{id}/ArrivalDepartures` is the only
  TfL endpoint that reports whether an individual train is delayed or cancelled, so a third request
  per refresh enriches the board with it. It looked at first like a straight replacement for
  `Arrivals` — smaller, and it carries scheduled *and* estimated times like a real board — and it is
  not:

  > **Every time field on it is named `...OfArrival`.** At a terminus you are originating, so there
  > is no arrival time and those fields come back EMPTY. Measured at Abbey Wood: 8 entries, 2 usable
  > departures, **neither with any time at all**. The same call at Liverpool Street, a through
  > station, gave 4 usable departures with full times. Switching wholesale would have halved the
  > morning board and stripped its clock times to gain a status column.

  So the board still comes from `Arrivals`; this endpoint only answers "is any of these cancelled".
  Consequences worth knowing before touching it:

  * **Coverage is partial by design**, and thinnest at Abbey Wood. `DepartureStatus::Unknown`
    therefore renders as a **blank**, never as "On time" — the board does not vouch for a train it
    was not told about.
  * **The two endpoints share no train identifier.** Matching is by destination naptan plus closest
    countdown, within `kStatusMatchSeconds` (150 s — they sample seconds apart and round
    differently, so exact equality never matches; core headways are ≥5 min so it cannot reach the
    adjacent train).
  * **`minutesAndSecondsToDeparture` is not zero-padded** — `"6:3"` is six minutes three seconds.
  * The presence of that field is what separates a departure from an arrival, which is a more direct
    rule than the naptan exclusion and is what the status parser uses.
  * The enrichment call uses **one attempt, not three**, and never writes `lastError()` — a failure
    must not mark a page degraded when everything a person reads off it is correct.
  * **`StatusTable::kMaxHints` was 16 and the first device run filled it exactly** (Liverpool Street
    returns ~21 entries across both directions and several branches). Entries are kept in feed
    order, so a cap below the station's total silently drops whichever trains sort late. Now 32.
    If a busier station is ever added, check this number against a live response first.
* **weather** — ✅ **DONE.** Open-Meteo behind a `WeatherProvider` interface, lat/lon from settings
  (no geocoding per refresh), °C + km/h, current/high/low/rain-probability/next six hours, 20 min
  refresh, cached response restored at boot. Parser is host-*testable* but not yet host-*tested*
  (§4.8 has no runner). Files:

  | File | Contents |
  | --- | --- |
  | `include/plugins/weather_model.hpp` + `src/weather_model.cpp` | **ESP-free and LVGL-free.** `Sky`, `WeatherHour`, `WeatherData`, WMO code mapping (`skyFromWmoCode`, `skyDescription`, `wmoDescription`), 16-point `windCompass()`, and `parseOpenMeteo(json, len, now_utc, out)`. `now_utc` is an argument, not a clock read, so hour selection is deterministic. |
  | `include/plugins/weather_provider.hpp` + `src/open_meteo.cpp` | `WeatherQuery`/`WeatherProvider` seam plus `OpenMeteoProvider`. Builds the URL, validates coordinates before spending a handshake on them, keeps the raw body for the caller to cache, and maps failures onto short user-facing reasons. 6 KB response buffer against a measured 2.2 KB response. |
  | `include/plugins/weather_plugin.hpp` + `src/weather_plugin.cpp` | The page. No HTTP or JSON in it. Hero temperature at 250 %, details card (high/low, feels like, rain, wind, humidity, sunrise/sunset), six hour chips. `setLocation()` refetches only when the coordinates actually change. |

  **The API's timestamp semantics were established by querying it, not from the documentation** —
  see the comment block at the top of `weather_model.hpp`. Every `unixtime` value is a true UTC
  epoch second; the documented "add `utc_offset_seconds` to daily timestamps" rule is **not** what
  the service does, and following it would have put sunrise an hour out. `timezone=auto` is still
  needed, because it decides that the daily high/low buckets are cut on the *local* calendar day.

  One addition to the framework came out of this: `PluginBase::noteCachedData(fetched_utc)`. Without
  it the state machine believed a cache-restoring plugin had never held data, so a failed first
  refresh reported "Unavailable" over a screen full of numbers.
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

1. ✅ ~~`dashboard_core`, `main/`, green build, working display~~ — all done.
2. **Investigate the watchdog reset loop** (top of this file). Cheap and it may already be gone.
   Flash, leave it running a few minutes, and check. If it resets: `idf.py coredump-info`.
3. **`components/dashboard_storage`** (§4.2) — settings and NVS come before everything that
   needs configuration, which is everything.
4. `components/dashboard_network` (§4.3) — Wi-Fi is the next big hardware unknown (the ESP32-C6
   link is entirely unproven), and it unblocks NTP, which unblocks a genuinely useful clock.
5. `dashboard_ota` (§4.4).
6. Real plugins in the order in §4.5: elizabeth_line → weather → todos → claude → settings, then
   the wallpaper lock (§4.5a). Delete each `PlaceholderPlugin` from `app_main.cpp` as its real
   plugin lands.
7. CI workflows (§4.7), host tests (§4.8), remaining docs (§4.9).

**Build AND flash after each component.** There is now a known-good baseline on real hardware,
which is worth far more than a green build alone. A regression caught immediately is a
five-minute fix; caught three components later it is an evening.

**Re-enable the parked optimisations** (§ "Display bring-up decisions", item 10) once things are
stable — one at a time, checking the display after each.
