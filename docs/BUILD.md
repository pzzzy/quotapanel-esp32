# Building and distributing

Python 3.11+ and a C++ compiler are needed for host tests. PlatformIO downloads the ESP32 toolchain automatically.

```sh
python -m pip install -r requirements.txt
python -m unittest discover -s tests -v
python -m platformio run -d firmware
python tools/package_release.py
```

The output is `dist/firmware`. Package only bootloader.bin, partitions.bin and firmware.bin with their manifest, SHA256SUMS and licenses. Do not distribute a full-chip readback, ELF/debug file, local cache or credentials. The flash tool checks image names, offsets, partition bounds and hashes before touching hardware, then requires a verified private backup.

## Pinned inputs

| Input | Version |
| --- | --- |
| PlatformIO Core | 6.2.0 |
| Espressif32 platform | 7.1.3 |
| Arduino ESP32 framework package | 4.20017.260907+sha.dcc1105b |
| Framework source revision | dcc1105b0cf1322a437b354c336f2abf72b7e512 |
| Xtensa ESP32-S3 compiler | 8.4.0+2021r2-patch5 |
| LovyanGFX | 1.2.30 |
| ArduinoJson | 7.4.2 |

The checked-in PCM header is the firmware input. Optional voice regeneration requires `requirements-voices.txt` and the exact model/config hashes in `assets/voice-model.json`; see assets/README.md. Normal builds do not download a speech model.

CI runs host tests, a clean firmware build, asset checks and release packaging. Build paths/timestamps and toolchain host differences may prevent byte-for-byte identity; checksums identify a particular artifact, not an unproven reproducible-build guarantee.

## Corresponding source

Every binary release must be accompanied by the project's tagged source and its `dependencies-source.tar.gz` archive. The latter contains the exact installed framework, platform build scripts and libraries used by the build, including their upstream notices. Toolchain/system components retain their own upstream source/license information. The source and build scripts permit modification, rebuilding and flashing without a signing secret or vendor authorization.

`tools/package_sources.py` creates this dependency archive from PlatformIO's installed packages and the environment's library directory. Do not substitute personal device backups for build outputs. See licenses/NOTICE.md for upstream provenance and terms.

Install `requirements-dev.txt` to run `python -m black --check tools tests`. CI checks Python formatting as well as behavior.
