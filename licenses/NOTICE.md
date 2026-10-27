# Third-party notices and source provenance

QuotaPanel ESP32 project code is Copyright 2026 QuotaPanel ESP32 contributors, GPL-3.0-only (root LICENSE), except files carrying their own license notice. This project was extracted from a private hardware prototype into a fresh repository; no emulator ROM, operating-system image, vendor flash dump or personal authentication material is distributed.

| Component | License / source |
| --- | --- |
| LovyanGFX 1.2.30 | MIT and included BSD notices; [upstream](https://github.com/lovyan03/LovyanGFX/tree/1.2.30); LovyanGFX.txt |
| ArduinoJson 7.4.2 | MIT; [upstream](https://github.com/bblanchon/ArduinoJson/tree/v7.4.2); ArduinoJson.txt |
| Arduino ESP32 framework | LGPL-2.1-or-later and component-specific notices; [exact revision](https://github.com/espressif/arduino-esp32/tree/dcc1105b0cf1322a437b354c336f2abf72b7e512); Arduino-ESP32-LGPL-2.1.txt |
| ESP-IDF and bundled libraries | Apache-2.0 and upstream component-specific terms, retained in the framework distribution |
| ES8311 adapter | Apache-2.0; adapted from Espressif esp-adf [es8311.c at 1c7aa251b937742f1308c4f3b9e7587034bc5fb7](https://github.com/espressif/esp-adf/blob/1c7aa251b937742f1308c4f3b9e7587034bc5fb7/components/esp_codec_dev/device/es8311/es8311.c); Apache-2.0.txt |
| Adafruit GFX font support | BSD; Adafruit-fonts.txt |
| GNU FreeFont-derived FreeSans glyphs | GPL-3.0-or-later with font exception; [GNU FreeFont licensing](https://www.gnu.org/software/freefont/license.html); root GPL text and upstream notices in the source bundle |
| Mozilla root store via certifi 2026.10.23 | MPL-2.0; [certifi](https://github.com/certifi/python-certifi); Certifi-MPL-2.0.txt; original PEM in assets/ca-roots.pem and generated bundle/provenance in native_roots.h and assets/ca-bundle.json |
| Speech clips and dashboard illustration | CC0-1.0; CC0-1.0.txt and assets/README.md |

The ES8311 adaptation replaces the general codec abstraction with a small fixed 22,050 Hz DAC-only register sequence and readback checks. Original Espressif attribution is retained. It does not use or distribute the prototype's audio-driver library.

Build/setup tools are dependencies, not embedded project code: PlatformIO (Apache-2.0), pyserial (BSD), Pillow (HPND), esptool (GPL-2.0-or-later); optional Piper generation has its own upstream GPL terms. Toolchain components retain their own licenses and runtime exceptions. Consult the installed package notices for their full terms.

Each binary release accompanies source and exact dependency snapshots as described in docs/BUILD.md. Upstream copyright notices and component licenses remain in those snapshots; this table does not replace them. OpenAI, ChatGPT and Codex are names of third-party products, not project endorsements. The AliExpress URL identifies the source of the tested hardware, not an affiliation.
