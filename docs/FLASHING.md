# Flashing the M5Stack Tab5 over USB

Step-by-step, from a clean machine to firmware running on the device.

Everything here has been verified on Windows 11 with ESP-IDF v5.4.4, **except the steps that
require the device itself** — those are marked ⚠️ **UNVERIFIED** and are written from M5Stack's
documentation. Please correct this file once you have run them for real.

---

## 1. What you need

| | |
| --- | --- |
| **Hardware** | M5Stack Tab5, plus a USB-C **data** cable |
| **Software** | ESP-IDF v5.4.4 — that is all |
| **Drivers** | **None.** See below. |

### No USB driver is required

Older ESP32 boards used a CP210x or CH340 USB-to-serial bridge chip and needed a vendor driver.
**The Tab5 does not.** Its USB-C port is wired to the ESP32-P4's own USB peripheral, so Windows 11
enumerates it as a standard COM port using the in-box driver.

If someone tells you to install a CP210x/CH340 driver for this board, they are thinking of a
different one.

### The cable matters

Many USB-C cables are **charge-only** and carry no data lines. If the device charges but no COM
port appears, suspect the cable before anything else. A USB-A-to-USB-C cable from a PC port is
the most reliable combination, because it forces the PC to be the USB host.

### Installing ESP-IDF

If you have not already:

1. Download the **Universal Online Installer** from <https://dl.espressif.com/dl/esp-idf/>.
   Do not use an offline installer — at the time of writing there is no offline package for the
   5.4 line that matches what this project pins.
2. Select **ESP-IDF v5.4.4**.
3. Accept the default install path `C:\Espressif` (no spaces, comfortably under the installer's
   90-character limit).
4. Make sure **esp32p4** is among the selected targets.

It installs its own Python 3.11, the RISC-V cross-compiler, CMake, Ninja, OpenOCD and esptool.
Do not install Python separately — the installer ignores your system Python, and ESP-IDF 5.4
does not support the newest releases anyway.

---

## 2. Open a terminal with ESP-IDF active

`idf.py` is not on your normal `PATH`. **Pick whichever of these matches the window you are in —
the commands are not interchangeable between cmd.exe and PowerShell.**

### Option A — the Start-menu shortcut (simplest)

The ESP-IDF installer creates two shortcuts, and **either one already has the environment set
up**. If you see this message, you are ready and need no further setup:

```
Done! You can now compile ESP-IDF projects.
Go to the project directory and run:  idf.py build
```

| Shortcut | Shell | Then run |
| --- | --- | --- |
| *ESP-IDF 5.4.4 Command Prompt (cmd.exe)* | **cmd.exe** | `cd "c:\Moreno Functions\Projects\M5Dash"` then `idf.py build` |
| *ESP-IDF 5.4.4 PowerShell* | **PowerShell** | same |

⚠️ **In the cmd.exe window, do NOT run `. .\scripts\idf_env.ps1`.** That is PowerShell syntax and
cmd will answer `'.' is not recognized as an internal or external command`. You do not need it —
the shortcut already exported everything.

### Option B — an ordinary PowerShell window

Use the helper script in this repository:

```powershell
cd "c:\Moreno Functions\Projects\M5Dash"
. .\scripts\idf_env.ps1
```

Note the **leading dot-space**: it must be dot-sourced so the environment variables survive the
call. Add `-Verify` to print the resolved tool versions — expect
`riscv32-esp-elf-gcc 14.2.0`, `cmake 3.30.2`, `ninja 1.12.1`, `Python 3.11.2`, `esptool.py v4.12.0`.

> **Why this script exists.** ESP-IDF ships its own `export.ps1`, but it invokes a bare `python`,
> which Windows 11 resolves to the Microsoft Store app-execution-alias stub. That stub prints
> `Python was not found...` and exits, and `export.ps1` then fails with
> `The expression after '.' in a pipeline element produced an object that was not valid`,
> leaving `IDF_PATH` empty. It looks like a broken ESP-IDF install and is not.
> `scripts/idf_env.ps1` calls the virtualenv's `python.exe` by absolute path instead.
> The cmd.exe path uses `export.bat`, which does not have this problem.

### Option C — an ordinary cmd.exe window

```cmd
C:\Espressif\idf_cmd_init.bat
cd "c:\Moreno Functions\Projects\M5Dash"
```

### Harmless warning

This appears on every activation and refers to a diagnostic file the installer does not create:

```
WARNING: ... No such file or directory: '...idf5.4_py3.11_env\idf_version.txt'
```

---

## 3. Build the firmware

```powershell
idf.py set-target esp32p4      # first time only; downloads ~28 components, takes a few minutes
idf.py build
```

A successful build ends with the artefact list and a size report:

```
tab5-desk-dashboard.bin binary size 0x134e40 bytes.
Smallest app partition is 0x600000 bytes. 0x4cb1c0 bytes (80%) free.
Project build complete.
```

`set-target` is only needed once, or after you change target or delete `sdkconfig`.

---

## 4. Put the Tab5 into download mode

⚠️ **UNVERIFIED** — from M5Stack's documentation.

1. Connect the USB-C cable to the Tab5 and to your PC.
2. **Press and hold the Reset button for about 2 seconds**, until the internal **green LED starts
   blinking rapidly**.
3. Release. The device is now waiting for firmware.

The rapid green blink is the confirmation. If it does not blink, you are not in download mode and
the flash will fail.

---

## 5. Find the COM port

**PowerShell:**

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

**cmd.exe:**

```cmd
mode
```

...or, if `mode`'s output is too verbose, borrow PowerShell for one command:

```cmd
powershell -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
```

You should get something like `COM7`. Substitute your value everywhere below.

**Or skip this entirely:** omit `-p` and ESP-IDF auto-detects, provided only one device is
attached — `idf.py flash monitor`.

To see device names as well as port numbers (PowerShell):

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } | Select-Object Name
```

Run it with the cable unplugged and again plugged in — the port that appears is the Tab5.

---

## 6. Check the flash size **before** flashing

⚠️ **Do this first. It is not optional.**

```powershell
esptool.py --chip esp32p4 -p COM7 flash_id
```

Look for the `Detected flash size:` line. It **must say 16MB**.

`partitions.csv` and `sdkconfig.defaults` in this project both assume a 16 MB part, with two
6 MB OTA slots. Espressif's own example configuration for this board declares **4 MB**, and if
your device really has 4 MB the partition table is invalid — the app will not fit, and flashing
it could leave the device unbootable.

If it reports anything other than 16 MB, **stop** and raise it before continuing. Changing the
partition table later requires a USB reflash anyway, so it is far cheaper to get right now.

This command also confirms the whole chain works — cable, port, download mode — before you write
anything to flash.

---

## 7. Flash and watch it boot

```powershell
idf.py -p COM7 flash monitor
```

This writes four things:

| Offset | File | Purpose |
| --- | --- | --- |
| `0x2000` | `bootloader/bootloader.bin` | second-stage bootloader |
| `0x8000` | `partition_table/partition-table.bin` | the layout from `partitions.csv` |
| `0xf000` | `ota_data_initial.bin` | OTA slot selector, initialised to "boot ota_0" |
| `0x20000` | `tab5-desk-dashboard.bin` | the application |

`monitor` then attaches to the serial output so you can watch it start.

**Press `Ctrl` + `]` to exit the monitor.** (Not Ctrl-C, which is passed through to the device.)

### If you prefer to do it by hand

```powershell
python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset `
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x2000  build\bootloader\bootloader.bin `
  0x8000  build\partition_table\partition-table.bin `
  0xf000  build\ota_data_initial.bin `
  0x20000 build\tab5-desk-dashboard.bin
```

Or, from inside `build\`, use the argument file the build generates:

```powershell
python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset `
  write_flash "@flash_args"
```

---

## 8. What you should see

✅ **VERIFIED on hardware** (M5Stack Tab5 with an ST7121 panel, ESP32-P4 rev v1.3, 16 MB flash).

**On the serial monitor:**

```
I (xxx) app: Desk Dashboard v0.0.0-dev (73eb795)
I (xxx) app: free heap at boot: ...
I (xxx) board: display up: panel 720x1280, UI 1280x720 (rotated 90 deg)
I (xxx) rtc8130: RX8130CE attached at 0x32, stored time ...
I (xxx) plugin: clock: initialised
I (xxx) pages: registered 'clock' (in rotation)
...
I (xxx) pages: 6 pages built, 5 in rotation, showing 'clock'
I (xxx) app: dashboard running; free heap: ...
```

Two lines are worth reading carefully:

* **`board: display up: panel 720x1280`** — confirms the panel initialised. The BSP also logs
  which display controller it detected (ILI9881C or ST7123), which tells us your Tab5 revision.
* **`rtc8130: ...`** — if it says *no RX8130CE at 0x32*, the RTC is absent or on a different
  address and the clock will depend entirely on network time.

**On the screen:**

1. A brief dark boot screen with **Desk Dashboard** and the version and git SHA.
2. Then the **clock page**: large time, day and date in British long form
   (e.g. *Sunday 10 August 2026*).
3. The footer will say **"Unavailable - waiting for time sync"** until the clock is set. This is
   correct — Wi-Fi is not implemented yet, so unless the RTC already held a valid time, the
   device genuinely does not know what time it is. It shows `--:--` rather than pretending.

**Touch:**

* **Swipe left / right** — move between the five pages. Four of them are placeholders that say
  *"Not implemented yet"* with a description; that is expected at this stage.
* **Swipe down** — manual refresh.
* **Long press (about 1 second) on a blank area** — opens the Settings page (also a placeholder).
  Long press deliberately does nothing when it lands on a button or list row.
* Small dots at the bottom centre show your position in the rotation.

If gestures feel wrong — too sensitive, not sensitive enough — every threshold is in one place:
[`config/app_config.hpp`](../config/app_config.hpp), under *Gestures*.

---

## 9. Troubleshooting

### No COM port appears

1. Try a different cable. Charge-only USB-C cables are extremely common.
2. Make sure the device is in download mode (green LED blinking rapidly after a ~2 s Reset hold).
3. Try a USB-A port with a USB-A-to-C cable rather than C-to-C.
4. Check Device Manager for an unknown device.

### `A fatal error occurred: Failed to connect`

The device is not in download mode. Repeat step 4 — the rapid green blink is the confirmation.

### The screen stays black but the serial log looks fine

**This is the single most likely problem on a Tab5, and it is almost never your application
code.** M5Stack ship three display panels and Espressif's BSP supports only two; the **ST7121**
is mis-identified as an ST7123 and initialised wrongly.

The symptoms are maximally misleading:

* every `esp_lcd_*` call returns `ESP_OK` — MIPI-DSI command writes are fire-and-forget, so
  nothing can report that the panel refused its configuration
* the backlight lights normally
* the touch controller enumerates and reports its firmware version and 720×1280 extent
* **even `esp_lcd_dpi_panel_set_pattern()` shows nothing**, because the DSI hardware test
  pattern is emitted through the same PHY the panel never locked onto

This project handles it — `tab5_board::detectPanel()` reads the touch firmware-version register
(1 = ST7121, 3 = ST7123) and uses the vendored `esp_lcd_st7121` driver where needed. Check the
boot log:

```
I (xxxx) board: detected panel: ST7121
I (xxxx) board: ST7121 panel up: 720x1280, DSI 965 Mbps x2 lanes, DPI 70 MHz
```

**If you are debugging a dark screen in any other project, do this first:** flash M5Stack's own
firmware from their [releases page](https://github.com/m5stack/M5Tab5-UserDemo/releases)
(a merged image — `write_flash 0x0 <file>`). If their firmware displays and yours does not, the
hardware is proven good and the fault is in your panel configuration. It is one flash and it
eliminates an entire branch of the search. Your own firmware is restored with `idf.py flash`.

Full background: [`IMPLEMENTATION_PLAN.md §3.1`](IMPLEMENTATION_PLAN.md).

If the panel is correct and it is *still* dark, suspect the backlight: `backlight init failed` is
logged as a warning and is deliberately non-fatal, so the device boots with a dark panel rather
than not booting at all.

### The build fails after adding a component

Almost always a version conflict. This project already carries one such fix: `esp_lvgl_port`
2.9.0 uses an LVGL 9.3 symbol but declares a loose enough dependency that the solver will pick
9.2 and then fail to compile. See the comments in [`main/idf_component.yml`](../main/idf_component.yml).

### Starting completely fresh

```powershell
idf.py -p COM7 erase-flash     # erases EVERYTHING: firmware, settings, saved Wi-Fi, tasks
idf.py -p COM7 flash monitor
```

This is the hardest of hard resets and is the recovery path if configuration ever becomes
corrupt, or if you forget a lock-screen PIN once that feature exists.

To reset only the OTA slot selection without losing settings:

```powershell
idf.py -p COM7 erase-otadata
```

### Recovering Wi-Fi (ESP32-C6 slave firmware)

⚠️ **UNVERIFIED** — not yet reached, since Wi-Fi is not implemented.

The Tab5 has no radio of its own. Wi-Fi is provided by an ESP32-C6 coprocessor reached over SDIO
using `esp_hosted`, and the C6 must be running a **slave firmware image compatible with the host
`esp_hosted` version** this project pins (1.4.0).

Retail units ship with a compatible image. If `esp_wifi_init()` ever fails with an RPC or version
mismatch, the C6 needs reflashing rather than the SDIO pin configuration being changed — those
pins are copied verbatim from M5Stack's own working configuration and are correct. M5Stack ship
a slave image at `platforms/tab5/wifi_c6_fw` in their
[M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo) repository.

---

## 10. Once it is running

Firmware updates after the first USB flash can be delivered over the air — see
[OTA.md](OTA.md). USB flashing remains necessary for:

* the very first flash of a new device,
* any change to `partitions.csv`,
* recovery from a failed update that rollback could not handle.
