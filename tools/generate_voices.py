#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate redistributable offline clips with the pinned Piper LJ Speech model.
Install optional requirements-voices.txt, then pass the verified local model.
The checked-in clips/header are the reproducible firmware inputs; inference can
vary slightly between ONNX runtimes, so generation is intentionally explicit.
"""

import argparse, array, hashlib, json, subprocess, sys, wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PHRASES = {
    "primary": "Primary account.",
    "secondary": "Secondary account.",
    "exhausted": "Usage exhausted.",
    "reset": "Usage allowance reset.",
    "banked_used": "Banked reset used.",
    "banked_gained": "Banked reset gained.",
    "warning_10": "Warning. Ten percent or less of your allowance remains.",
    "warning_5": "Warning. Five percent or less of your allowance remains.",
}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model", type=Path, required=True)
    args = p.parse_args()
    meta = json.loads((ROOT / "assets/voice-model.json").read_text())
    for suffix, key in [("", "modelSha256"), (".json", "configSha256")]:
        data = Path(str(args.model) + suffix).read_bytes()
        if hashlib.sha256(data).hexdigest() != meta[key]:
            raise ValueError("Voice model checksum mismatch")
    out = ROOT / "assets/voice"
    out.mkdir(exist_ok=True)
    header = [
        "// SPDX-License-Identifier: CC0-1.0",
        "#pragma once",
        "#include <stdint.h>",
        "// Piper LJ Speech; 22050 Hz mono signed PCM. See assets/README.md.",
    ]
    manifest = {}
    for name, phrase in PHRASES.items():
        path = out / (name + ".wav")
        subprocess.run(
            [
                sys.executable,
                "-m",
                "piper",
                "--model",
                str(args.model),
                "--length-scale",
                "0.9",
                "--noise-scale",
                "0",
                "--noise-w-scale",
                "0",
                "--output-file",
                str(path),
            ],
            input=phrase + "\n",
            text=True,
            check=True,
        )
        with wave.open(str(path), "rb") as w:
            if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (22050, 1, 2):
                raise ValueError("Unexpected audio format")
            values = array.array("h", w.readframes(w.getnframes()))
        if sys.byteorder != "little":
            values.byteswap()
        audible = [i for i, x in enumerate(values) if abs(x) > 100]
        if not audible:
            raise ValueError("Empty speech")
        first = max(0, audible[0] - 220)
        last = min(len(values), audible[-1] + 441)
        values = values[first:last]
        peak = max(abs(x) for x in values)
        scale = 24575 / max(1, peak)
        for i, x in enumerate(values):
            values[i] = round(x * scale * min(1, i / 220, (len(values) - 1 - i) / 220))
        raw = (
            values.tobytes()
            if sys.byteorder == "little"
            else b"".join(int(x).to_bytes(2, "little", signed=True) for x in values)
        )
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(22050)
            w.writeframes(raw)
        header.append(f"static const int16_t voice_{name}[] PROGMEM = {{")
        for i in range(0, len(values), 24):
            header.append(",".join(map(str, values[i : i + 24])) + ",")
        header.append("};")
        manifest[name] = {
            "text": phrase,
            "seconds": round(len(values) / 22050, 3),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        }
        print(name, manifest[name]["seconds"], flush=True)
    (ROOT / "firmware/src/voices.generated.h").write_text("\n".join(header) + "\n")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
