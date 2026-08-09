# Third-party licences

This project is MIT licensed — see [LICENSE](LICENSE).

Everything below is a **dependency resolved at build time by the ESP-IDF component manager**.
None of it is vendored into this repository; `managed_components/` is git-ignored and is
recreated by `idf.py build`. This file exists so the licence position is knowable without
running a build.

The licences recorded here were read from the `LICENSE` file shipped inside each component after
a successful build on **2026-08-10**, not assumed. Re-check after any dependency version change.

---

## Build toolchain

| Component | Version | Licence |
| --- | --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) | v5.4.4 | Apache-2.0 |
| Bundled cJSON (ESP-IDF `json` component) | — | MIT |
| Bundled mbedTLS | — | Apache-2.0 |
| Bundled newlib, FreeRTOS, lwIP | — | various permissive (BSD/MIT/Apache-2.0) |

---

## Runtime dependencies

Directly declared in [`main/idf_component.yml`](main/idf_component.yml):

| Component | Version | Licence |
| --- | --- | --- |
| `espressif/m5stack_tab5` | 1.2.0~1 | Apache-2.0 |
| `lvgl/lvgl` | 9.3.0 | MIT (© LVGL Kft) |
| `espressif/esp_hosted` | 1.4.0 | Apache-2.0 |
| `espressif/esp_wifi_remote` | 0.8.5 | Apache-2.0 |
| `joltwallet/littlefs` | 1.22.3 | MIT (© Brian Pugh) — wraps upstream [littlefs](https://github.com/littlefs-project/littlefs), BSD-3-Clause |

Pulled in transitively, mostly as public dependencies of the Tab5 BSP:

| Component | Licence |
| --- | --- |
| `espressif/esp_lvgl_port` | Apache-2.0 |
| `espressif/esp_io_expander` | Apache-2.0 |
| `espressif/esp_io_expander_pi4ioe5v6408` | Apache-2.0 |
| `espressif/esp_lcd_ili9881c` | Apache-2.0 |
| `espressif/esp_lcd_st7123` | Apache-2.0 |
| `espressif/esp_lcd_touch` | Apache-2.0 |
| `espressif/esp_lcd_touch_gt911` | Apache-2.0 |
| `espressif/esp_lcd_touch_st7123` | Apache-2.0 |
| `espressif/esp_codec_dev` | Apache-2.0 |
| `espressif/esp_serial_slave_link` | Apache-2.0 |
| `espressif/eppp_link` | Apache-2.0 |
| `espressif/i2c_bus` | Apache-2.0 |
| `espressif/sensor_hub` | Apache-2.0 |
| `espressif/bmi270` | Apache-2.0 |
| `espressif/cmake_utilities` | Apache-2.0 |
| `espressif/usb`, `espressif/usb_host_uvc` | Apache-2.0 |
| `espressif/esp_cam_sensor`, `espressif/esp_h264`, `espressif/esp_sccb_intf` | Apache-2.0 |
| `espressif/esp_video` | MIT |
| `espressif/esp_ipa` | MIT |

Several of the above (camera, audio codec, USB host, IMU) arrive only because they are public
dependencies of the BSP. The dashboard does not use them.

---

## Fonts and icons

No font binary is committed to this repository. The glyphs the dashboard renders come from
**LVGL's pre-converted built-in fonts**, enabled by Kconfig in `sdkconfig.defaults`.

| Asset | Source | Licence |
| --- | --- | --- |
| Montserrat (sizes 12–48) | `Montserrat-Medium.ttf`, converted by LVGL | [SIL Open Font License 1.1](https://openfontlicense.org/) |
| `LV_SYMBOL_*` glyphs | Font Awesome 5 Free (Solid + Brands + Regular) | Icons: CC BY 4.0 · Font files: SIL OFL 1.1 |

Both are freely redistributable in a compiled binary, which is what the brief required.

### Character coverage — a practical constraint, not just a licensing note

LVGL's Montserrat faces are built with the glyph range:

```
0x20-0x7F   (printable ASCII)
0xB0        (° degree sign)
0x2022      (• bullet)
+ the Font Awesome symbol range used by LV_SYMBOL_*
```

**Anything outside that renders as an empty box on the device.** In particular em dashes (—),
ellipses (…) and middle dots (·) are *not* available. Degree signs are, so `°C` is fine.

Keep on-screen strings inside this range. Comments and log messages are unaffected.

If a wider range or a larger size is ever needed, generate a font with `lv_font_conv` from an
OFL-licensed face and record it here.

---

## Reference material

The following were **read** during development to establish hardware facts. No code was copied
from them, but they deserve acknowledgement:

| Source | Licence | Used for |
| --- | --- | --- |
| [m5stack/M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo) | MIT | Confirming the ESP-IDF version, and the verified ESP32-C6 SDIO pin map and reset line in `sdkconfig.defaults` |
| [espressif/esp-bsp](https://github.com/espressif/esp-bsp) | Apache-2.0 | Tab5 BSP API surface and initialisation order |
| Epson RX8130CE datasheet | — | RTC register map |
| Linux `rtc-rx8130` driver | GPL-2.0 | Cross-checking the RTC register map only. No code derived from it — the driver here is an independent implementation from the datasheet, deliberately, to avoid any GPL entanglement. |
