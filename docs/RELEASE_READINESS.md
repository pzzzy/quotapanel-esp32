# Release readiness

## Alpha.3 display defaults and runway persistence

The update boots in flipped landscape orientation with sound enabled at the saved volume. Both accounts returned live after verified flashing; more than 12 hours of existing forecast coverage survived the upgrade. The history record format and partition layout are unchanged.

45 host tests pass, including 370 C++/Python estimator comparisons and a production-journal harness under address/undefined-behavior sanitizers. The journal tests cover all 32 partial-record write lengths, corruption, sector and sequence rollover, storage failures, account/cycle isolation, and forecast reconstruction after reboot. Historical fallback and its uncertainty are heuristic; this is not a calibrated prediction-accuracy claim. See the release notes for the final physical display/touch and reboot observations.


**Suitable for an experimental open-source alpha on the documented board. Not yet a general-purpose appliance or stable release.**

## 0.1.0-alpha.2 recovery correction

A real power-off/restart incident exposed an endpoint certificate rotation, not lost account sessions. The previous two-root trust set rejected the new Google chain. A compact Mozilla bundle was also insufficient because the older SDK's callback mishandles the cross-signed chain. The correction uses complete Mozilla/certifi roots and the standard path verifier, with TLS memory in PSRAM.

Negative testing also revealed that the pinned SDK disables certificate-date verification. The new adapter checks the selected chain's UTC validity before allowing HTTP transmission, without clearing other verification errors. A fresh SNTP sync is required each boot. Diagnostic errors and retries are visible on the board; a definitely-unsent refresh can retry without invalidating the session.

The host suite now contains 37 tests, including trusted-root provenance, complete trust-set preservation, certificate-time boundaries, and conservative refresh/backoff policy. Hardware results for this correction are recorded in the release notes.

## Initial alpha evidence


- Standalone precursor: independent browser OAuth for both accounts, direct TLS usage/reset-credit reads, repeated on-device token refresh, reboot recovery and 28 successful polling cycles. The host bridge was removed after these checks. A five-minute retry-aware run had no polling failures or USB retries and stable idle heap.
- Public candidate: pinned clean local build, verified full backup, allowlisted flash writes and readback verification. The application preserves the established credential/history partitions.
- Python runtime dependency audit: no known vulnerabilities after upgrading Pillow to 13.0.0. This is a point-in-time package audit, not a firmware security certification.
- 34 host tests pass locally, including 353 generated C++/Python forecast comparisons under address/undefined-behavior sanitizers, reset/threshold cases, USB framing, audio checksums and protection against firmware overwriting the credential partition.
- The underlying alert detector previously passed 120 on-board synthetic parity cases; its algorithm is unchanged in this release.
- Public candidate stability run: 11 completed polling cycles, zero failed polls, ten USB samples over about five minutes, and no USB retries or audio failures. Idle heap snapshots varied by only 24 bytes. A software reboot returned to DIRECT in 14.5 seconds with audio ready. Both accounts independently refreshed their tokens and returned HTTP 200 for usage and reset credits.
- Public voice assets and fixed ES8311 adapter initialize on the real board. The user confirmed the replacement warning voice is clear at the stored volume.
- Public firmware configuration succeeds over framed USB, including Wi-Fi and timezone. Existing independent accounts reconnect. Fresh browser pairing was tested on the precursor; the new generic setup wrapper still needs a clean-device, independent-user walkthrough.
- Fresh repository, ignored private build/cache/backup areas, curated files, tracked-tree privacy scan, license notices and synthetic illustration. No personal screenshots or account data are intended for publication.

Post-flash USB checks exposed a repeatable need for an explicit USB-UART reset after the flasher exits. The flash tool now performs that reset and verifies the application version before reporting completion; this recovery path was tested on the board. Manual reset recovery remains documented for other USB/host combinations. A full restore was not destructively exercised against the currently configured device.

## Conditions before a stable release

1. Days-to-weeks unattended testing, including Wi-Fi outages, upstream failures, clock loss, expired sessions and power interruption during every persistence stage.
2. A clean-device setup walkthrough by someone other than the author, on supported operating systems; test wrong-account rejection and account removal offline.
3. Defined support policy for unofficial account endpoints and additional seller board revisions.
4. A credential-storage deployment option using flash encryption/secure boot, with an explicit recovery story. The alpha stores secrets unencrypted.
5. More real-world forecast evaluation. Current confidence labels are heuristics and ranges are sensitivity bounds, not calibrated prediction probabilities.

## Useful next features

- One-account operation, custom labels and adjustable quiet hours.
- A visible reauthorization-needed state with a guided setup path.
- Signed, recoverable OTA updates (requires a deliberate partition migration).
- Optional battery telemetry and ambient/idle dimming after validating the actual board circuitry.
- Provider adapters with explicit, supported authentication/data contracts.

Do not add microphone capture, purchases, reset redemption, or broad cloud permissions merely because hardware or an endpoint makes them possible. They are separate product decisions.
