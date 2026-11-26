#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""QuotaPanel ESP32 USB setup and diagnostics. No host token storage or bridge."""

import argparse, getpass, hashlib, hmac, json, os, subprocess, sys, time, webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlsplit, parse_qs
import serial
from serial.tools import list_ports


class Device:
    def __init__(self, port=None):
        self.port = port or select_port()
        self.wire = None
        self.buffer = bytearray()

    def __enter__(self):
        self.wire = serial.Serial(self.port, 115200, timeout=0.2, write_timeout=5)
        time.sleep(0.6)
        self.wire.reset_input_buffer()
        return self

    def __exit__(self, *_):
        self.wire.close()

    def line(self, deadline):
        while time.monotonic() < deadline:
            end = self.buffer.find(b"\n")
            if end >= 0:
                row = bytes(self.buffer[:end]).strip()
                del self.buffer[: end + 1]
                if row:
                    return row
            self.buffer.extend(self.wire.read(max(1, self.wire.in_waiting)))
            if len(self.buffer) > 100000:
                raise RuntimeError("Oversized USB response")
        raise TimeoutError("Display did not reply")

    def send(self, command):
        self.wire.write(("\n" + command + "\n").encode())

    def expect(self, command, reply):
        self.send(command)
        end = time.monotonic() + 8
        while time.monotonic() < end:
            row = self.line(end)
            if row == reply:
                return
            if row.startswith(b"ERROR"):
                raise RuntimeError(row.decode(errors="replace"))
        raise TimeoutError("Display did not acknowledge USB command")

    def query(self, command, key):
        for attempt in range(3):
            self.send(command)
            end = time.monotonic() + 5
            try:
                while time.monotonic() < end:
                    row = self.line(end)
                    if not row.startswith(b"{"):
                        continue
                    try:
                        value = json.loads(row)
                    except ValueError:
                        continue
                    if key in value:
                        return value
            except TimeoutError:
                pass
        raise TimeoutError("No matching diagnostic response")

    def transfer(self, command, reply=b"OK NATIVE QUEUED"):
        payload = command.encode()
        if not 0 < len(payload) <= 8192:
            raise ValueError("USB command exceeds the board limit")
        self.expect(f"NBEGIN {len(payload)} {checksum(payload):08x}", b"OK BEGIN")
        for offset in range(0, len(payload), 32):
            chunk = payload[offset : offset + 32]
            self.expect(
                f"NCHUNK {offset} {chunk.hex()}",
                f"OK CHUNK {offset+len(chunk)}".encode(),
            )
        self.expect("NCOMMIT", reply)

    def native(self, command, timeout=120):
        self.transfer(command)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            time.sleep(0.5)
            result = self.query("NATIVE RESULT", "state")
            if result["state"] in ("queued", "idle", "running"):
                continue
            if result.get("error"):
                raise RuntimeError(result["error"])
            if result["state"] in ("wifi_unavailable", "clock_unavailable"):
                raise RuntimeError(result["state"])
            return result
        raise TimeoutError("Device operation did not finish")


def checksum(data):
    value = 2166136261
    for b in data:
        value = ((value ^ b) * 16777619) & 0xFFFFFFFF
    return value


def select_port():
    ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
    if len(ports) != 1:
        raise RuntimeError(
            "Specify --port; use the ports command to list connected devices."
        )
    return ports[0]


def validate_network(ssid, password, timezone):
    if not 0 < len(ssid.encode()) <= 32:
        raise ValueError("SSID must be 1–32 UTF-8 bytes")
    if len(password.encode()) > 63:
        raise ValueError("Wi-Fi password exceeds 63 bytes")
    allowed = set(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-:,/."
    )
    if not 0 < len(timezone) <= 96 or not set(timezone) <= allowed:
        raise ValueError("Use a POSIX timezone such as UTC0 or EST5EDT,M3.2.0,M11.1.0")
    return {"ssid": ssid, "password": password, "timezone": timezone}


def pair(port, slot, no_browser, expected):
    callback = {}
    state = ""
    label = ("PRIMARY", "SECONDARY")[slot]

    class Handler(BaseHTTPRequestHandler):
        timeout = 5

        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.headers.get("Host") not in ("127.0.0.1:1455", "localhost:1455"):
                self.send_error(400)
                return
            url = urlsplit(self.path)
            q = parse_qs(url.query)
            if url.path == "/auth/callback":
                received = q.get("state", [""])[0]
                if (
                    callback
                    or not state
                    or not received.isascii()
                    or not hmac.compare_digest(received, state)
                ):
                    self.send_error(400, "Invalid state")
                    return
                if q.get("error"):
                    callback["error"] = "Authorization was declined"
                elif q.get("code"):
                    callback.update(code=q["code"][0], state=state)
                else:
                    self.send_error(400, "Missing code")
                    return
                self.send_response(303)
                self.send_header("Location", "/done")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Referrer-Policy", "no-referrer")
                self.end_headers()
                return
            if url.path == "/done":
                message = (
                    "Authorization declined. Return to the setup tool to retry."
                    if callback.get("error")
                    else f"Return to the setup tool for the {label} result."
                )
                body = f'<html><title>QuotaPanel ESP32</title><body style="font:22px system-ui;padding:3em"><h1>QuotaPanel ESP32</h1><p>{message}</p></body></html>'.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_error(404)

    try:
        server = HTTPServer(("127.0.0.1", 1455), Handler)
    except OSError as exc:
        raise RuntimeError(
            "Cannot open 127.0.0.1:1455. Finish any other local sign-in and try again."
        ) from exc
    server.timeout = 1
    try:
        with Device(port) as device:
            request = device.native(
                "BROWSER " + json.dumps({"slot": slot, "expected": expected or ""})
            )
        state = request["stateToken"]
        url = request["authorizationUrl"]
        parsed = urlsplit(url)
        if (
            parsed.scheme != "https"
            or parsed.hostname != "auth.openai.com"
            or parsed.path != "/oauth/authorize"
        ):
            raise RuntimeError("Unexpected authorization URL")
        print(f"Authorize {label} (account {slot+1}) in your browser.", flush=True)
        if no_browser or not webbrowser.open(url):
            print(
                f"Open this link in the browser profile for {label}:\n{url}", flush=True
            )
        end = time.monotonic() + 880
        while not callback and time.monotonic() < end:
            server.handle_request()
        if not callback:
            raise TimeoutError("Authorization expired; run pair again")
        server.handle_request()
    finally:
        server.server_close()
    if callback.get("error"):
        raise RuntimeError(callback["error"])
    with Device(port) as device:
        try:
            result = device.native("CALLBACK " + json.dumps(callback))
        except RuntimeError as exc:
            if str(exc) in (
                "exchange_or_account_mismatch",
                "account_already_paired_in_other_slot",
            ):
                raise RuntimeError(
                    f"{label} was not changed. Check the signed-in account and retry from its browser profile or a private window."
                ) from exc
            raise
    callback.clear()
    if not result.get("persisted"):
        raise RuntimeError(f"{label} sign-in was not saved")
    print(f"{label} sign-in saved on the ESP32.", flush=True)
    return result


def account_targets(connection):
    return [
        index
        for index, account in enumerate(connection.get("accounts", [])[:2])
        if account.get("reauthRequired", account.get("error") == "SIGN IN AGAIN")
    ]


def wait_for_accounts(device, targets, after_poll, timeout=120):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        connection = device.query("NATIVE STATUS", "state")
        accounts = connection.get("accounts", [])
        if connection.get("polls", 0) > after_poll and all(
            len(accounts) > slot and accounts[slot].get("ok") for slot in targets
        ):
            return True
        time.sleep(1)
    return False


def reauth(port, account, no_browser):
    with Device(port) as device:
        status = device.query("STATUS", "firmware")
        if not status.get("standalone"):
            raise RuntimeError(
                "Direct mode is disabled. Use pair and enable for setup."
            )
        if not status.get("wifi"):
            raise RuntimeError("The ESP32 needs a Wi-Fi connection to sign in.")
        if account is None:
            connection = device.query("NATIVE STATUS", "state")
            if len(connection.get("accounts", [])) != 2:
                raise RuntimeError(
                    "Wait for the board's first poll, or specify --account."
                )
            targets = account_targets(connection)
        else:
            targets = [account - 1]
    if not targets:
        print(
            "No account has requested reauthorization. Use diagnose for connection errors."
        )
        return
    for slot in targets:
        pair(port, slot, no_browser, expected=None)
    with Device(port) as device:
        after_poll = device.query("NATIVE STATUS", "state").get("polls", 0)
        # Older firmware needs a nudge to exit its accumulated retry backoff.
        device.native("MODE 1")
        if not wait_for_accounts(device, targets, after_poll):
            raise RuntimeError(
                "Sign-in saved. The board has not verified fresh usage and credits yet. Run diagnose."
            )
    print("Live usage and banked resets verified for the recovered accounts.")


def screenshot(device, output):
    from PIL import Image

    if output.exists():
        raise FileExistsError(output)
    device.expect("CAPTURE", b"OK CAPTURE")
    pixels = bytearray()
    for y in range(240):
        for attempt in range(4):
            device.send(f"ROW {y}")
            end = time.monotonic() + 4
            try:
                while time.monotonic() < end:
                    fields = device.line(end).split()
                    if len(fields) != 4 or fields[:2] != [b"ROW", str(y).encode()]:
                        continue
                    row = bytes.fromhex(fields[2].decode())
                    if len(row) == 960 and checksum(row) == int(fields[3], 16):
                        pixels.extend(row)
                        break
                else:
                    continue
                if len(pixels) == (y + 1) * 960:
                    break
            except (TimeoutError, ValueError):
                continue
        else:
            raise RuntimeError(f"Framebuffer row {y} failed validation")
    Image.frombytes("RGB", (320, 240), bytes(pixels)).save(output)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", default=os.environ.get("QUOTAPANEL_PORT"))
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("ports")
    sub.add_parser("status")
    sub.add_parser("data")
    sub.add_parser("enable")
    sub.add_parser("reboot")
    sub.add_parser("diagnose")
    sub.add_parser("tls-check")
    sub.add_parser("reconnect")
    config = sub.add_parser("configure")
    config.add_argument("--ssid")
    config.add_argument("--timezone", default="UTC0")
    auth = sub.add_parser("pair")
    auth.add_argument("--account", type=int, choices=[1, 2], required=True)
    auth.add_argument("--expected-account")
    auth.add_argument("--no-browser", action="store_true")
    recover = sub.add_parser(
        "reauth", help="recover accounts that need browser sign-in"
    )
    recover.add_argument("--account", type=int, choices=[1, 2])
    recover.add_argument("--no-browser", action="store_true")
    refresh = sub.add_parser("refresh")
    refresh.add_argument("--account", type=int, choices=[1, 2], required=True)
    forget = sub.add_parser("forget")
    forget.add_argument("--account", type=int, choices=[1, 2], required=True)
    screen = sub.add_parser("screenshot")
    screen.add_argument("output", type=Path)
    vol = sub.add_parser("volume")
    vol.add_argument("percent", type=int, choices=range(101))
    view = sub.add_parser("view")
    view.add_argument("page", type=int, choices=[-1, 0, 1])
    alert = sub.add_parser("alert")
    alert.add_argument(
        "kind",
        choices=[
            "exhausted",
            "reset",
            "banked_used",
            "banked_gained",
            "warning_10",
            "warning_5",
        ],
    )
    alert.add_argument("--account", type=int, choices=[1, 2], default=1)
    args = p.parse_args()
    if args.command == "ports":
        for item in list_ports.comports():
            print(item.device, item.description)
        return
    port = args.port or select_port()
    if args.command == "pair":
        pair(port, args.account - 1, args.no_browser, args.expected_account)
        print("For initial setup, pair both accounts, then run enable.")
        return
    if args.command == "reauth":
        reauth(port, args.account, args.no_browser)
        return
    with Device(port) as device:
        if args.command == "configure":
            net = validate_network(
                args.ssid or input("2.4 GHz Wi-Fi SSID: "),
                getpass.getpass("Wi-Fi password (empty for open network): "),
                args.timezone,
            )
            device.transfer("CONFIG " + json.dumps(net), b"OK CONFIG")
            print("Settings saved. Display is restarting.")
        elif args.command == "status":
            print(json.dumps(device.query("STATUS", "firmware"), indent=2))
        elif args.command == "diagnose":
            print(
                json.dumps(
                    {
                        "device": device.query("STATUS", "firmware"),
                        "connection": device.query("NATIVE STATUS", "state"),
                    },
                    indent=2,
                )
            )
        elif args.command in ("tls-check", "reconnect"):
            print(
                json.dumps(
                    device.native(
                        "TLSCHECK" if args.command == "tls-check" else "RECONNECT"
                    ),
                    indent=2,
                )
            )
        elif args.command == "data":
            print(json.dumps(device.query("DATA", "source"), indent=2))
        elif args.command == "enable":
            print(json.dumps(device.native("MODE 1"), indent=2))
        elif args.command == "refresh":
            result = device.native(f"REFRESH {args.account-1}")
            if not result.get("refreshed"):
                raise RuntimeError("Token refresh did not complete.")
            reads = {r["endpoint"]: r["http"] for r in result.get("requests", [])}
            if reads.get("usage") != 200 or reads.get("reset-credits") != 200:
                raise RuntimeError(
                    "Token refreshed, but usage or banked resets could not be read."
                )
            print(
                "Token refreshed on the ESP32. Usage and banked resets returned HTTP 200."
            )
        elif args.command == "forget":
            if (
                input(f"Type FORGET to remove account {args.account} from this board: ")
                != "FORGET"
            ):
                raise SystemExit("Cancelled")
            device.native(
                "FORGET "
                + json.dumps({"slot": args.account - 1, "confirm": "FORGET_ACCOUNT"})
            )
            device.expect("REBOOT", b"OK REBOOT")
            print(
                "Account removed; direct polling paused until both accounts are paired."
            )
        elif args.command == "reboot":
            device.expect("REBOOT", b"OK REBOOT")
        elif args.command == "screenshot":
            screenshot(device, args.output)
            print(args.output)
        elif args.command == "volume":
            device.expect(f"VOLUME {args.percent}", b"OK VOLUME")
        elif args.command == "view":
            device.expect(f"VIEW {args.page}", b"OK VIEW")
        elif args.command == "alert":
            kind = [
                "exhausted",
                "reset",
                "banked_used",
                "banked_gained",
                "warning_10",
                "warning_5",
            ].index(args.kind)
            device.expect(f"ALERT {kind} {args.account-1}", b"OK ALERT")


if __name__ == "__main__":
    try:
        main()
    except (
        RuntimeError,
        TimeoutError,
        ValueError,
        serial.SerialException,
        OSError,
    ) as error:
        raise SystemExit(str(error))
