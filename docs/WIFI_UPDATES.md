# Signed Wi-Fi updates

After one USB migration and pairing, updates run directly over your local Wi-Fi. No always-running Mac process, cloud bridge, router port forwarding or physical button press is required. Only application firmware is updated over Wi-Fi; bootloader or partition-layout changes still require USB. Updates are manually initiated; the board does not automatically install GitHub releases.

## One-time installation

Install alpha.4 or later using the documented backup/flash tool. The USB bundle adds OTA metadata and two application slots in previously unused flash. Accounts, Wi-Fi settings and history retain their existing addresses. The USB installer clears OTA boot selection so the newly written factory application runs. Keep the verified full-flash backup private.

Then pair a unique update identity over the trusted USB connection:

```sh
python tools/wifi_update.py pair --profile .local/ota-device.json
python tools/wifi_update.py status --profile .local/ota-device.json
```

Pairing creates a unique TLS certificate/private key for the board and a random 256-bit access credential. The local profile is created with owner-only permissions before configuration is sent, so an interrupted reply cannot lose a credential that may already have committed. The profile contains the credential and certificate fingerprint; keep it private and back it up securely. If pairing is interrupted, reboot and try `status` before deliberately re-pairing with a new profile path. A new pairing replaces the old update identity.

The updater uses the board's unique `quotapanel-….local` mDNS name. If local name resolution is unavailable, supply `--host <board-IP>`; the same certificate pin is still mandatory. The initial address is also saved as `lastAddress` in the private profile. On Linux, `.local` resolution may require your distribution's mDNS resolver. No USB connection is needed to change the address used by the client.

## Installing an update

Download the `.qpu` asset from a release. Use the updater and public verification key from the repository:

```sh
python tools/wifi_update.py upload quotapanel-esp32-v0.1.0-alpha.4.qpu --profile .local/ota-device.json
```

The computer verifies the signature, complete image hash and release sequence before uploading. The board independently checks the signature before erasing an inactive slot, checks the streamed hash and ESP image structure, then selects that slot and reboots. Keep power connected while it updates. The tool waits for the new firmware to confirm its health; HTTP acceptance alone is not reported as success. If reboot interrupts the final acknowledgement, it checks the authenticated running version and health without blindly resending the image.

The first boot must run its display loop, start the HTTPS update service, initialize audio, and complete two successful two-account polls after at least 30 seconds. Until then, the image remains pending. A failed boot or a three-minute health timeout rolls back through the bootloader. A 30-second loop watchdog also detects a stalled main task. Before an update or health-timeout rollback reboots, the native account worker is paused under a mutex so an in-flight rotating-token request can finish. Waiting for that request can extend the timeout by up to 90 seconds per attempt; preserving an uncertain token rotation takes precedence over forced reboot.

The factory application remains a recovery copy; subsequent updates alternate between `ota_0` and `ota_1`. Interrupted uploads leave the boot selection unchanged. Updating never writes account or history partitions. A failed NVS initialization does not erase accounts or the update identity.

## Security properties and limits

- TLS 1.2 or newer encrypts transport. The updater checks the exact USB-paired certificate fingerprint **before sending HTTP headers or the credential**. This is explicit certificate pinning, not trust-on-first-use, and there is no bypass option. The certificate is a pinned device identity rather than a public-PKI hostname identity.
- A device-specific bearer credential authorizes both status and update requests. The board stores its SHA-256 digest. No browser-based configuration or account-data endpoint is exposed. Listen port: HTTPS 8443; keep it on a trusted LAN, without public port forwarding.
- ECDSA P-256/SHA-256 binds the board identifier, monotonically increasing release sequence, version, image size and SHA-256 digest. The firmware contains only the release **public** key. A leaked device profile permits update requests but cannot sign arbitrary firmware.
- The sequence must exceed both the running firmware's sequence and its stored accepted-release floor. Replays and signed downgrades are rejected. This is application-level downgrade protection, not an irreversible eFuse counter. Bootloader rollback to a previously working image remains available.
- Physical/USB access is still trusted. This project does not burn eFuses or enable hardware secure boot/flash encryption. Physical access, a compromised signing key, or a vulnerability in an already trusted firmware can defeat these application-level protections. TLS resource limits and timeouts reduce exposure but do not prevent all LAN denial of service.
- Loss of the local profile or replacement of its pinned identity requires deliberate USB re-pairing. Loss of the signing key requires a trusted key-transition release signed by the old key, or USB replacement of the trust key. Do not erase working trust material casually.

## Maintainer signing

The release private key is kept outside version control, separate from device profiles. Public key: `assets/ota-public-key.pem`; matching compiled key: `firmware/src/ota_public_key.h`. Keep an offline backup of the signing key. It is intentionally not a CI secret or release asset.

For every release, increment `QP_SEQUENCE` in `firmware/src/firmware_version.h`, update `QP_VERSION`, build, test, then sign the exact application binary:

```sh
python tools/wifi_update.py sign firmware/.pio/build/quotapanel-esp32/firmware.bin \
  --sequence 107 --version 0.1.0-alpha.5 \
  --key /private/path/release-key.pem \
  --output dist/quotapanel-esp32-v0.1.0-alpha.5.qpu
```

The example's sequence/version must match the compiled release. The first boot rejects mismatched embedded release metadata. Never reuse a sequence for a different image. Publish the `.qpu`, checksums, corresponding project/dependency sources, and USB recovery bundle. Ordinary CI can build and test without the private key; signing is a separate maintainer operation.

Fork owners should generate their own P-256 key, replace both public-key files, and USB-install that trust anchor before distributing their own signed updates. A key-transition release must be signed with the old key (use `sign --verification-key old-public-key.pem` and `upload --public-key old-public-key.pem`) and contain the new public key; retain rollback/recovery copies and consider how their trust anchors will be updated.

## Format and flash layout

A `.qpu` has 112 bytes of signed metadata, a 64-byte raw ECDSA `r || s` signature, and the complete application image. Metadata uses little-endian integers: eight-byte magic `QPOTA001`, 32-byte zero-padded board identifier, 32-bit sequence, 32-bit image size, 32-byte image SHA-256, and 32-byte zero-padded version. The board identifier is `quotapanel-esp32s3-v1`. OTA images are limited to 4 MiB; the USB factory/recovery application remains limited to 2 MiB. HTTP chunked requests and mismatched lengths are rejected.

| Region | Offset | Size |
| --- | --- | --- |
| Existing settings | `0x9000` | 20 KiB |
| Factory/recovery application | `0x10000` | 2 MiB |
| Existing accounts and OTA identity | `0x210000` | 64 KiB |
| Existing history | `0x220000` | 64 KiB |
| OTA boot metadata | `0x230000` | 8 KiB |
| OTA application 0 | `0x240000` | 4 MiB |
| OTA application 1 | `0x640000` | 4 MiB |

Rollback follows the pinned framework's [ESP-IDF OTA state machine](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/system/ota.html). Arduino's automatic early confirmation is overridden; application health decides when to accept the new slot.
