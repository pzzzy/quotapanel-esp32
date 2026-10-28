#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check tracked release inputs for accidental private artifacts and broken links.
This is a guardrail, not a substitute for reviewing the staged diff.
"""

import re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    names = (
        subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
        .decode()
        .split("\0")
    )
    issues = []
    patterns = [
        (
            re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
            "private key",
        ),
        (
            re.compile(
                r"eyJ[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{10,}"
            ),
            "JWT",
        ),
        (
            re.compile(r"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})"),
            "GitHub token",
        ),
        (
            re.compile(r"/(?:Users|home)/[A-Za-z][A-Za-z0-9_.-]*/"),
            "personal absolute path",
        ),
    ]
    for name in filter(None, names):
        path = ROOT / name
        if (
            set(path.relative_to(ROOT).parts)
            & {".local", ".venv", ".pio", "__pycache__"}
            or path.suffix in (".bin", ".elf", ".sqlite", ".db")
            or path.name in ("auth.json", "wifi.json")
        ):
            issues.append(name + ": private/build artifact")
            continue
        if path.suffix == ".wav":
            continue
        text = path.read_text(errors="replace")
        if (
            path.suffix == ".json"
            and '"certificateSha256"' in text
            and '"token"' in text
        ):
            issues.append(name + ": private OTA device profile")
        for pattern, label in patterns:
            if pattern.search(text):
                issues.append(name + ": " + label)
        if path.suffix == ".md":
            for ref in re.findall(r"\]\(([^)]+)\)", text):
                if "://" in ref or ref.startswith("#"):
                    continue
                target = ref.split("#")[0]
                if target and not (path.parent / target).exists():
                    issues.append(name + ": missing link " + target)
    if issues:
        raise SystemExit("\n".join(issues))
    print("Tracked-tree privacy and documentation checks passed.")


if __name__ == "__main__":
    main()
