#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Archive the public build dependencies, never a project workspace or flash dump."""

import argparse, json, os, tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser()
    p.add_argument(
        "--core-dir",
        type=Path,
        default=Path(
            os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")
        ),
    )
    p.add_argument(
        "--output", type=Path, default=ROOT / "dist/dependencies-source.tar.gz"
    )
    args = p.parse_args()
    sources = [
        (
            args.core_dir / "packages/framework-arduinoespressif32",
            "framework-arduinoespressif32",
        ),
        (args.core_dir / "platforms/espressif32", "platform-espressif32"),
        (ROOT / "firmware/.pio/libdeps/quotapanel-esp32/LovyanGFX", "LovyanGFX"),
        (ROOT / "firmware/.pio/libdeps/quotapanel-esp32/ArduinoJson", "ArduinoJson"),
    ]
    for path, _ in sources:
        if not path.is_dir():
            raise SystemExit("Build first; dependency missing: " + str(path))
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def clean(info):
        if any(
            part in (".git", "__pycache__") for part in Path(info.name).parts
        ) or info.name.endswith((".pyc", "/.DS_Store")):
            return None
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info

    with tarfile.open(args.output, "w:gz") as archive:
        for path, name in sources:
            archive.add(path, arcname=name, filter=clean)
    print(args.output)


if __name__ == "__main__":
    main()
