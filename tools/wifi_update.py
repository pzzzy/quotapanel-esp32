#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Signed QuotaPanel updates over certificate-pinned HTTPS; no runtime bridge."""

import argparse
import datetime
import hashlib
import hmac
import http.client
import json
import os
import re
import secrets
import ssl
import struct
import time
from pathlib import Path

from cryptography import x509
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.x509.oid import NameOID
from quotapanel import Device

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_KEY = ROOT / "assets/ota-public-key.pem"
FORMAT = struct.Struct("<8s32sII32s32s")
HEADER_SIZE = FORMAT.size + 64
MAX_IMAGE = 0x400000
BOARD = b"quotapanel-esp32s3-v1"


def private_write(path, data):
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as out:
        out.write(data)


def metadata(image, sequence, version):
    if not 0 < sequence <= 0xFFFFFFFF or not 256 <= len(image) <= MAX_IMAGE:
        raise ValueError("Invalid sequence or image size")
    if not re.fullmatch(r"[A-Za-z0-9.-]{1,31}", version):
        raise ValueError("Invalid release version")
    return FORMAT.pack(
        b"QPOTA001",
        BOARD,
        sequence,
        len(image),
        hashlib.sha256(image).digest(),
        version.encode(),
    )


def sign(image, sequence, version, key):
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(
        key.curve, ec.SECP256R1
    ):
        raise ValueError("Expected a P-256 signing key")
    header = metadata(image, sequence, version)
    r, s = utils.decode_dss_signature(key.sign(header, ec.ECDSA(hashes.SHA256())))
    return header + r.to_bytes(32, "big") + s.to_bytes(32, "big") + image


def verify(bundle, public_key, floor=0):
    if len(bundle) < HEADER_SIZE:
        raise ValueError("Truncated update")
    magic, board, sequence, size, digest, raw_version = FORMAT.unpack(
        bundle[: FORMAT.size]
    )
    if (
        magic != b"QPOTA001"
        or board != BOARD.ljust(32, b"\0")
        or sequence <= floor
        or not 256 <= size <= MAX_IMAGE
    ):
        raise ValueError("Wrong board, invalid size, or obsolete sequence")
    if (
        len(bundle) != HEADER_SIZE + size
        or hashlib.sha256(bundle[HEADER_SIZE:]).digest() != digest
    ):
        raise ValueError("Firmware hash/length mismatch")
    version = raw_version.rstrip(b"\0").decode("ascii")
    if not re.fullmatch(r"[A-Za-z0-9.-]{1,31}", version):
        raise ValueError("Invalid version")
    r = int.from_bytes(bundle[112:144], "big")
    s = int.from_bytes(bundle[144:176], "big")
    if not isinstance(public_key, ec.EllipticCurvePublicKey) or not isinstance(
        public_key.curve, ec.SECP256R1
    ):
        raise ValueError("Expected a P-256 verification key")
    public_key.verify(
        utils.encode_dss_signature(r, s), bundle[:112], ec.ECDSA(hashes.SHA256())
    )
    return {
        "sequence": sequence,
        "version": version,
        "bytes": size,
        "sha256": digest.hex(),
    }


class IdentityError(ssl.SSLError):
    pass


class PinnedHTTPS(http.client.HTTPSConnection):
    def __init__(self, host, fingerprint, timeout=180):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        super().__init__(host, port=8443, timeout=timeout, context=context)
        self.fingerprint = fingerprint

    def connect(self):
        super().connect()
        # Exact certificate pin is checked before http.client sends HTTP headers
        # or the device access credential. No TOFU or bypass option exists.
        actual = hashlib.sha256(self.sock.getpeercert(binary_form=True)).hexdigest()
        if not hmac.compare_digest(actual, self.fingerprint):
            self.close()
            raise IdentityError(
                "Device certificate does not match its USB-paired identity"
            )


def request(profile, method, route, body=None, host=None, timeout=180):
    connection = PinnedHTTPS(
        host or profile["hostname"], profile["certificateSha256"], timeout
    )
    try:
        connection.request(
            method,
            route,
            body,
            {
                "Authorization": "Bearer " + profile["token"],
                "Content-Type": "application/octet-stream",
                "Connection": "close",
            },
        )
        connection.sock.settimeout(min(timeout, 30))
        response = connection.getresponse()
        data = response.read(16385)
        if len(data) > 16384:
            raise RuntimeError("Oversized device response")
        result = json.loads(data)
        if response.status not in (200, 202):
            raise RuntimeError(
                f"Device rejected request ({response.status}): {result.get('error', 'unknown')}"
            )
        return result
    finally:
        connection.close()


def pair(path, port):
    if path.exists():
        raise FileExistsError(
            "Device profile exists; keep it for updates. Use a new path to deliberately re-pair."
        )
    with Device(port) as device:
        status = device.query("OTA STATUS", "hostname")
        network = device.query("STATUS", "firmware")
        hostname = status["hostname"] + ".local"
        key = ec.generate_private_key(ec.SECP256R1())
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, hostname)])
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(
                x509.SubjectAlternativeName([x509.DNSName(hostname)]), critical=False
            )
            .sign(key, hashes.SHA256())
        )
        token = secrets.token_hex(32)
        profile = {
            "hostname": hostname,
            "lastAddress": network["ip"],
            "certificateSha256": cert.fingerprint(hashes.SHA256()).hex(),
            "token": token,
        }
        config = {
            "certificate": cert.public_bytes(serialization.Encoding.PEM).decode(),
            "privateKey": key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            ).decode(),
            "authorizationHash": hashlib.sha256(token.encode()).hexdigest(),
        }
        # Save credentials before sending: an interrupted setup never loses the
        # credential for a configuration which may already have committed.
        private_write(path, json.dumps(profile, indent=2) + "\n")
        device.transfer("OTA SETUP " + json.dumps(config), b"OK OTA SETUP")
        device.expect("REBOOT", b"OK REBOOT")
    print(f"Paired. Keep {path} private; future updates use Wi-Fi.")
    return profile


def upload(profile, bundle, public_key, host=None):
    before = request(profile, "GET", "/v1/status", host=host, timeout=10)
    info = verify(bundle, public_key, floor=before["minimumSequence"])
    try:
        request(profile, "POST", "/v1/update", bundle, host=host)
    except IdentityError:
        raise
    except (OSError, http.client.HTTPException):
        # A reboot may cut the final acknowledgement after activation. Never
        # resend blindly: authenticate the running version and health instead.
        print(
            "Upload acknowledgement interrupted; checking the device directly…",
            flush=True,
        )
    print("Waiting for the signed version to boot and confirm its health…", flush=True)
    started = time.monotonic()
    while time.monotonic() - started < 300:
        time.sleep(3)
        try:
            status = request(profile, "GET", "/v1/status", host=host, timeout=5)
        except IdentityError:
            raise
        except (OSError, http.client.HTTPException, ValueError):
            continue
        if status["sequence"] == info["sequence"]:
            if (
                not status["pendingVerify"]
                and status["state"] == "ready"
                and status["accountsHealthy"]
            ):
                if status["version"] != info["version"]:
                    raise RuntimeError("Running version differs from signed metadata")
                print(
                    f"Verified {status['version']} on {status['runningSlot']}; both accounts healthy."
                )
                return status
        elif (
            time.monotonic() - started > 30
            and not status.get("updating", status["state"] != "ready")
            and not status["pendingVerify"]
        ):
            raise RuntimeError(
                "Update rolled back or did not boot; previous firmware remains active"
            )
    raise TimeoutError(
        "No health confirmation; check Wi-Fi status before retrying. Do not assume success."
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("pair")
    p.add_argument("--port")
    p.add_argument("--profile", type=Path, default=ROOT / ".local/ota-device.json")
    for name in ("status", "upload"):
        p = sub.add_parser(name)
        p.add_argument("--profile", type=Path, default=ROOT / ".local/ota-device.json")
        p.add_argument(
            "--host",
            help="Override address after a DHCP change; the certificate pin still applies",
        )
        if name == "upload":
            p.add_argument("bundle", type=Path)
            p.add_argument("--public-key", type=Path, default=PUBLIC_KEY)
    p = sub.add_parser("sign")
    p.add_argument("firmware", type=Path)
    p.add_argument("--sequence", type=int, required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--key", type=Path, required=True)
    p.add_argument(
        "--verification-key",
        type=Path,
        default=PUBLIC_KEY,
        help="Currently trusted public key; specify the old key for an intentional trust-key transition",
    )
    p.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "pair":
        pair(args.profile, args.port)
    elif args.command == "sign":
        key = serialization.load_pem_private_key(args.key.read_bytes(), password=None)
        bundle = sign(args.firmware.read_bytes(), args.sequence, args.version, key)
        # The release signer must match the verification key in this source tree.
        verify(
            bundle,
            serialization.load_pem_public_key(args.verification_key.read_bytes()),
        )
        with args.output.open("xb") as out:
            out.write(bundle)
        print(args.output)
    else:
        profile = json.loads(args.profile.read_text())
        if args.command == "status":
            print(
                json.dumps(
                    request(profile, "GET", "/v1/status", host=args.host), indent=2
                )
            )
        else:
            upload(
                profile,
                args.bundle.read_bytes(),
                serialization.load_pem_public_key(args.public_key.read_bytes()),
                args.host,
            )


if __name__ == "__main__":
    try:
        main()
    except (
        OSError,
        ValueError,
        RuntimeError,
        InvalidSignature,
        http.client.HTTPException,
    ) as error:
        raise SystemExit(str(error) or "Firmware signature verification failed")
