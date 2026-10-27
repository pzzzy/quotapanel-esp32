#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package only allowlisted firmware files; never include device flash or NVS."""

import argparse, hashlib, json, shutil, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path, default=ROOT / "dist/firmware")
    args = p.parse_args()
    build = ROOT / "firmware/.pio/build/quotapanel-esp32"
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    meta = {
        "project": "QuotaPanel ESP32",
        "version": "0.1.0-alpha.1",
        "chip": "esp32s3",
        "flashBytes": 16777216,
        "files": {},
    }
    for name, offset in [
        ("bootloader.bin", 0),
        ("partitions.bin", 0x8000),
        ("firmware.bin", 0x10000),
    ]:
        data = (build / name).read_bytes()
        (out / name).write_bytes(data)
        meta["files"][name] = {
            "offset": offset,
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    (out / "manifest.json").write_text(json.dumps(meta, indent=2) + "\n")
    (out / "SHA256SUMS").write_text(
        "".join(f"{v['sha256']}  {k}\n" for k, v in meta["files"].items())
    )
    shutil.copy2(ROOT / "LICENSE", out / "LICENSE")
    shutil.copytree(ROOT / "licenses", out / "licenses", dirs_exist_ok=True)
    (out / "SOURCE.txt").write_text(
        "Complete project source, build instructions and dependency versions: https://github.com/pzzzy/quotapanel-esp32\nSee docs/BUILD.md and licenses/NOTICE.md. No secure boot or flashing restriction is imposed.\n"
    )
    print(out)


if __name__ == "__main__":
    main()
