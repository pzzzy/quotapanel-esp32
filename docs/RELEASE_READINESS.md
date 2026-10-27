# Release readiness: 0.1.0-alpha.1

**Suitable for an experimental open-source alpha on the documented board. Not yet a general-purpose appliance or stable release.**

## Evidence

- Standalone precursor: independent browser OAuth for both accounts, direct TLS usage/reset-credit reads, repeated on-device token refresh, reboot recovery and 28 successful polling cycles. The host bridge was removed after these checks. A five-minute retry-aware run had no polling failures or USB retries and stable idle heap.
- Public candidate: pinned clean local build, verified full backup, allowlisted flash writes and readback verification. The application preserves the established credential/history partitions.
- 34 host tests pass locally, including 353 generated C++/Python forecast comparisons under address/undefined-behavior sanitizers, reset/threshold cases, USB framing, audio checksums and protection against firmware overwriting the credential partition.
- The underlying alert detector previously passed 120 on-board synthetic parity cases; its algorithm is unchanged in this release.
- Public voice assets and fixed ES8311 adapter initialize on the real board. The user confirmed the replacement warning voice is clear at the stored volume.
- Public firmware configuration succeeds over framed USB, including Wi-Fi and timezone. Existing independent accounts reconnect. Fresh browser pairing was tested on the precursor; the new generic setup wrapper still needs a clean-device, independent-user walkthrough.
- Fresh repository, ignored private build/cache/backup areas, curated files, tracked-tree privacy scan, license notices and synthetic illustration. No personal screenshots or account data are intended for publication.

One post-flash USB status attempt timed out; an explicit hardware reset restored normal startup. This is recorded rather than counted as a clean first-boot success. The setup guide includes reset recovery. A full restore was not destructively exercised against the currently configured device.

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
