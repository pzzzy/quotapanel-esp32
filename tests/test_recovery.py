import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from generate_ca_bundle import build_bundle, outputs
from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding


class Recovery(unittest.TestCase):
    def test_refresh_retry_is_only_allowed_before_request_sent(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / "recovery")
            subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-fsanitize=address,undefined",
                    str(ROOT / "tests/recovery_harness.cpp"),
                    "-o",
                    exe,
                ],
                check=True,
            )
            subprocess.run([exe], check=True)

    def test_ca_bundle_matches_pinned_input(self):
        for path, expected in outputs().items():
            self.assertEqual(path.read_text(), expected, path.name)

    def test_full_certificates_preserve_the_complete_trust_set(self):
        pem = (ROOT / "assets/ca-roots.pem").read_bytes()
        bundle, roots = build_bundle(pem)
        generated = x509.load_pem_x509_certificates(bundle)
        self.assertEqual(
            {c.public_bytes(Encoding.DER) for c in generated},
            {c.public_bytes(Encoding.DER) for c in roots},
        )
        names = {c.subject.rfc4514_string() for c in generated}
        self.assertTrue(any("ISRG Root X1" in n for n in names))
        self.assertTrue(any("GTS Root R4" in n for n in names))
        meta = json.loads((ROOT / "assets/ca-bundle.json").read_text())
        self.assertEqual(meta["bundleSha256"], hashlib.sha256(bundle).hexdigest())


if __name__ == "__main__":
    unittest.main()
