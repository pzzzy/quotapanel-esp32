#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verified backups and flashing for the supported 16 MiB ESP32-S3 board.
Backups can contain credentials: keep them private and out of issue reports.
"""

import argparse, hashlib, json, os, subprocess, sys, time
from pathlib import Path
from quotapanel import Device, select_port

ROOT = Path(__file__).resolve().parents[1]
SIZE = 16 * 1024 * 1024


def esp(port, arguments, before="default-reset", after="hard-reset", capture=False):
    return subprocess.run(
        [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            "esp32s3",
            "--port",
            port,
            "--baud",
            "921600",
            "--before",
            before,
            "--after",
            after,
            *arguments,
        ],
        check=True,
        text=True,
        capture_output=capture,
    )


def check_hardware(port):
    result = esp(port, ["flash-id"], capture=True)
    if "16MB" not in result.stdout and "16 MB" not in result.stdout:
        raise RuntimeError("Expected 16 MiB flash; refusing to continue")
    print(
        "ESP32-S3 with 16 MiB flash detected. Confirm the panel/pinout against docs/HARDWARE.md."
    )


def backup(port, directory):
    directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    os.chmod(directory, 0o700)
    image = directory / "full-flash.bin"
    image.touch(mode=0o600)
    esp(port, ["read-flash", "0x0", hex(SIZE), str(image)], after="no-reset")
    if image.stat().st_size != SIZE:
        raise RuntimeError("Incomplete flash backup")
    esp(
        port,
        [
            "verify-flash",
            "--flash-mode",
            "keep",
            "--flash-freq",
            "keep",
            "--flash-size",
            "keep",
            "0x0",
            str(image),
        ],
        before="no-reset",
        after="hard-reset",
    )
    meta = {
        "bytes": SIZE,
        "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "verified": True,
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    path = directory / "manifest.json"
    path.write_text(json.dumps(meta, indent=2) + "\n")
    path.chmod(0o600)
    print(f"Verified private backup: {directory}")


def verify_backup(directory):
    image = directory / "full-flash.bin"
    meta = json.loads((directory / "manifest.json").read_text())
    if (
        not meta.get("verified")
        or image.stat().st_size != SIZE
        or hashlib.sha256(image.read_bytes()).hexdigest() != meta["sha256"]
    ):
        raise RuntimeError("Backup checksum/size verification failed")
    return image


def verify_bundle(directory):
    meta = json.loads((directory / "manifest.json").read_text())
    expected = {"bootloader.bin": 0, "partitions.bin": 0x8000, "firmware.bin": 0x10000}
    if (
        meta.get("chip") != "esp32s3"
        or meta.get("flashBytes") != SIZE
        or set(meta.get("files", {})) != set(expected)
    ):
        raise RuntimeError("Unexpected release manifest")
    args = []
    limits = {
        "bootloader.bin": 0x8000,
        "partitions.bin": 0x1000,
        "firmware.bin": 0x200000,
    }
    for name, offset in expected.items():
        entry = meta["files"][name]
        path = directory / name
        if (
            not 0 < path.stat().st_size <= limits[name]
            or entry.get("offset") != offset
            or path.stat().st_size != entry["bytes"]
            or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]
        ):
            raise RuntimeError("Release checksum/offset mismatch: " + name)
        args.extend([hex(offset), str(path)])
    return args


def boot_and_verify(port):
    # Explicit USB-UART reset is required on the tested board after the stub
    # flasher exits. Keep GPIO0 released, pulse EN, then verify the application.
    with Device(port) as device:
        device.wire.dtr = False
        device.wire.rts = True
        time.sleep(0.15)
        device.wire.rts = False
        time.sleep(2)
        result = device.query("STATUS", "firmware")
        if not result["firmware"].startswith("quotapanel-"):
            raise RuntimeError("Unexpected application after flashing")
        print("Application started: " + result["firmware"])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port")
    sub = p.add_subparsers(dest="command", required=True)
    save = sub.add_parser("backup")
    save.add_argument("directory", type=Path)
    restore = sub.add_parser("restore")
    restore.add_argument("directory", type=Path)
    flash = sub.add_parser("flash")
    flash.add_argument("bundle", type=Path)
    flash.add_argument("--backup", type=Path, required=True)
    args = p.parse_args()
    port = args.port or select_port()
    # All local integrity checks precede touching the device.
    image = verify_backup(args.directory) if args.command == "restore" else None
    write_args = verify_bundle(args.bundle) if args.command == "flash" else None
    check_hardware(port)
    if args.command == "backup":
        backup(port, args.directory)
        return
    if args.command == "restore":
        if (
            input(
                "Type RESTORE to replace all flash, including credentials and settings: "
            )
            != "RESTORE"
        ):
            raise SystemExit("Cancelled")
        esp(
            port,
            [
                "write-flash",
                "--flash-mode",
                "keep",
                "--flash-freq",
                "keep",
                "--flash-size",
                "keep",
                "0x0",
                str(image),
            ],
            after="no-reset",
        )
        esp(
            port,
            [
                "verify-flash",
                "--flash-mode",
                "keep",
                "--flash-freq",
                "keep",
                "--flash-size",
                "keep",
                "0x0",
                str(image),
            ],
            before="no-reset",
        )
        return
    if (
        input("Type FLASH to install QuotaPanel ESP32 on the documented board: ")
        != "FLASH"
    ):
        raise SystemExit("Cancelled")
    backup(port, args.backup)
    esp(
        port,
        [
            "write-flash",
            "--flash-mode",
            "keep",
            "--flash-freq",
            "keep",
            "--flash-size",
            "keep",
            *write_args,
        ],
        after="no-reset",
    )
    esp(
        port,
        [
            "verify-flash",
            "--flash-mode",
            "keep",
            "--flash-freq",
            "keep",
            "--flash-size",
            "keep",
            *write_args,
        ],
        before="no-reset",
    )
    boot_and_verify(port)
    print(
        "Firmware verified. Next: configure Wi-Fi, pair both accounts, enable direct mode."
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
