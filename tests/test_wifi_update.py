import hashlib
import json
import ssl
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch, MagicMock
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from wifi_update import sign, verify, PinnedHTTPS, private_write, upload, IdentityError
from flash import verify_bundle


class WifiUpdate(unittest.TestCase):
    def setUp(self):
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.bundle = sign(bytes(range(256)), 100, "0.1.0-alpha.4", self.key)

    def test_compiled_and_distributed_public_keys_match(self):
        public = (ROOT / "assets/ota-public-key.pem").read_text()
        header = (ROOT / "firmware/src/ota_public_key.h").read_text()
        self.assertIn(public, header)
        self.assertNotIn("PRIVATE KEY", header)

    def test_signature_image_and_metadata_are_all_bound(self):
        self.assertEqual(verify(self.bundle, self.key.public_key())["sequence"], 100)
        for position in (0, 8, 40, 44, 48, 80, 112, 144, 176, len(self.bundle) - 1):
            corrupted = bytearray(self.bundle)
            corrupted[position] ^= 1
            with self.assertRaises((ValueError, InvalidSignature)):
                verify(bytes(corrupted), self.key.public_key())
        with self.assertRaises(InvalidSignature):
            verify(self.bundle, ec.generate_private_key(ec.SECP256R1()).public_key())
        for bad in (self.bundle[:-1], self.bundle + b"x", self.bundle[:100]):
            with self.assertRaises(ValueError):
                verify(bad, self.key.public_key())

    def test_replay_downgrade_bounds_and_version(self):
        for floor in (100, 101, 0xFFFFFFFF):
            with self.assertRaises(ValueError):
                verify(self.bundle, self.key.public_key(), floor)
        for seq, ver, size in (
            (0, "a", 256),
            (2**32, "a", 256),
            (1, "a", 255),
            (1, "a", 0x400001),
            (1, "a" * 32, 256),
            (1, "bad\x00x", 256),
        ):
            with self.assertRaises(ValueError):
                sign(b"x" * size, seq, ver, self.key)

    def test_native_metadata_parser_matches_signed_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "update.qpu"
            bundle.write_bytes(self.bundle)
            exe = str(Path(tmp) / "format")
            subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-fsanitize=address,undefined",
                    str(ROOT / "tests/ota_format_harness.cpp"),
                    "-o",
                    exe,
                ],
                check=True,
            )
            subprocess.run([exe, str(bundle)], check=True)

    def test_pin_checked_before_any_http_credential_is_sent(self):
        pin = hashlib.sha256(b"trusted cert").hexdigest()
        conn = PinnedHTTPS("device.invalid", pin)
        sock = MagicMock()
        sock.getpeercert.return_value = b"attacker cert"

        def connect(_):
            conn.sock = sock

        with patch("http.client.HTTPSConnection.connect", connect):
            with self.assertRaises(ssl.SSLError):
                conn.request(
                    "GET", "/v1/status", headers={"Authorization": "Bearer private"}
                )
        sock.sendall.assert_not_called()
        sock.close.assert_called_once()

    def test_lost_upload_acknowledgement_checks_health_without_resending(self):
        ready = {
            "sequence": 100,
            "version": "0.1.0-alpha.4",
            "pendingVerify": False,
            "state": "ready",
            "accountsHealthy": True,
            "runningSlot": "ota_0",
        }
        with (
            patch(
                "wifi_update.request",
                side_effect=[{"minimumSequence": 99}, TimeoutError("reboot"), ready],
            ) as requests,
            patch("wifi_update.time.sleep"),
        ):
            self.assertEqual(upload({}, self.bundle, self.key.public_key()), ready)
            self.assertEqual(
                [call.args[1] for call in requests.call_args_list],
                ["GET", "POST", "GET"],
            )

    def test_identity_failure_is_not_treated_as_a_lost_ack(self):
        with patch(
            "wifi_update.request",
            side_effect=[{"minimumSequence": 99}, IdentityError("wrong pin")],
        ) as requests:
            with self.assertRaises(IdentityError):
                upload({}, self.bundle, self.key.public_key())
            self.assertEqual(requests.call_count, 2)

    def test_private_profile_never_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "profile.json"
            private_write(path, "test")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                private_write(path, "replacement")
            self.assertEqual(path.read_text(), "test")

    def test_migration_bundle_only_resets_ota_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            meta = {
                "chip": "esp32s3",
                "flashBytes": 16777216,
                "layout": "ota-v1",
                "files": {},
            }
            for name, offset in (
                ("bootloader.bin", 0),
                ("partitions.bin", 0x8000),
                ("firmware.bin", 0x10000),
                ("ota-data-reset.bin", 0x230000),
            ):
                data = b"\xff" * 8192 if name == "ota-data-reset.bin" else b"x" * 32
                (root / name).write_bytes(data)
                meta["files"][name] = {
                    "offset": offset,
                    "bytes": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                }
            (root / "manifest.json").write_text(json.dumps(meta))
            self.assertEqual(verify_bundle(root)[-2], "0x230000")
            meta["files"]["ota-data-reset.bin"]["offset"] = 0x210000
            (root / "manifest.json").write_text(json.dumps(meta))
            with self.assertRaises(RuntimeError):
                verify_bundle(root)
