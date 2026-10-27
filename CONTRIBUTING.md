# Contributing

Small, focused pull requests are welcome. Explain the observed problem, the change, and how you verified it. Run the host tests and firmware build described in docs/BUILD.md. Use synthetic fixtures; never commit credentials, account responses, personal screenshots, flash readbacks or vendor firmware images.

For a new board, provide a separate explicit profile, schematic/pinout provenance and hardware validation. Do not broaden the supported-hardware claim based only on an ESP32 model match. Changes to token rotation, persistence, rate parsing, forecasting and alert detection need regression tests for the actual failure mode.

Treat internal endpoint changes defensively: missing values remain unknown, stale data stays stale, and retries must not replay uncertain rotating credentials. Do not add reset redemption, billing actions or microphone capture as incidental side effects.

Dependencies and GitHub Actions are pinned. Include license/provenance updates for new code, fonts and media. Project contributions are GPL-3.0-only unless an existing file has a different explicit license. Do not copy Apple system-voice recordings or proprietary firmware into this repository.

Be respectful, assume good intent, and keep discussion focused on the work. Security reports belong in the private channel described in SECURITY.md.
