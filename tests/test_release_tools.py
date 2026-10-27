import hashlib, json, sys, tempfile, unittest, wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from flash import verify_bundle
from quotapanel import Device, checksum, validate_network


class ReleaseSafety(unittest.TestCase):
    def bundle(self, root, extra=False, oversized=False):
        files = {}
        for name, offset in [
            ("bootloader.bin", 0),
            ("partitions.bin", 0x8000),
            ("firmware.bin", 0x10000),
        ]:
            data = b"x" * (0x200001 if oversized and name == "firmware.bin" else 32)
            (root / name).write_bytes(data)
            files[name] = {
                "offset": offset,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        if extra:
            files["accounts.bin"] = {}
        (root / "manifest.json").write_text(
            json.dumps({"chip": "esp32s3", "flashBytes": 16777216, "files": files})
        )

    def test_bundle_valid(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            self.bundle(root)
            self.assertEqual(verify_bundle(root)[::2], ["0x0", "0x8000", "0x10000"])

    def test_bundle_corruption(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            self.bundle(root)
            (root / "firmware.bin").write_bytes(b"bad")
            with self.assertRaises(RuntimeError):
                verify_bundle(root)

    def test_extra_partition_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            self.bundle(root, extra=True)
            with self.assertRaises(RuntimeError):
                verify_bundle(root)

    def test_application_cannot_overwrite_accounts(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            self.bundle(root, oversized=True)
            with self.assertRaises(RuntimeError):
                verify_bundle(root)

    def test_network_utf8_limits(self):
        self.assertEqual(validate_network("é" * 16, "", "UTC0")["ssid"], "é" * 16)
        for args in [
            ("é" * 17, "", "UTC0"),
            ("net", "x" * 64, "UTC0"),
            ("net", "", "America/New_York"),
            ("net", "", "UTC0\n"),
        ]:
            with self.assertRaises(ValueError):
                validate_network(*args)

    def test_usb_transfer_roundtrip(self):
        device = Device("unused")
        calls = []
        device.expect = lambda command, reply: calls.append((command, reply))
        command = "CONFIG " + json.dumps(
            {"ssid": "a" * 32, "password": "b" * 63, "timezone": "UTC0"}
        )
        device.transfer(command, b"OK CONFIG")
        payload = command.encode()
        rebuilt = bytearray()
        self.assertEqual(calls[0][0], f"NBEGIN {len(payload)} {checksum(payload):08x}")
        for line, ack in calls[1:-1]:
            _, offset, hexdata = line.split()
            self.assertEqual(int(offset), len(rebuilt))
            chunk = bytes.fromhex(hexdata)
            self.assertLessEqual(len(chunk), 32)
            rebuilt.extend(chunk)
            self.assertEqual(ack, f"OK CHUNK {len(rebuilt)}".encode())
        self.assertEqual(bytes(rebuilt), payload)
        self.assertEqual(calls[-1], ("NCOMMIT", b"OK CONFIG"))

    def test_audio_integrity(self):
        manifest = json.loads((ROOT / "assets/voice/manifest.json").read_text())
        for name, meta in manifest.items():
            path = ROOT / "assets/voice" / f"{name}.wav"
            self.assertEqual(
                hashlib.sha256(path.read_bytes()).hexdigest(), meta["sha256"]
            )
            with wave.open(str(path)) as wav:
                self.assertEqual(
                    (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()),
                    (22050, 1, 2),
                )


if __name__ == "__main__":
    unittest.main()
