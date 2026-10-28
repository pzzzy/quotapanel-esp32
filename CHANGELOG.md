# Changelog

## 0.1.0-alpha.3

- Rotate the default display 180° with matching touch coordinates. Boot unmuted at the saved volume; mute lasts until reboot.
- Include recent quiet/low-consumption horizons in the runway rate so slowdowns are reflected earlier.
- Reuse recent saved trends after an offline gap, clearly labeled as past pace with low confidence and wider age-dependent bounds. Require fresh allowance data; discard pre-correction slopes and expire fallback trends after six hours.
- Preserve the existing history format; checkpoint significant usage changes sooner. Exercise the production journal against reboot, all partial-write lengths, corruption, sector wrap, sequence wrap and storage failures. Surface history-save failures without discarding live readings.

## 0.1.0-alpha.2

- Fix usage retrieval after a server certificate rotates from Let's Encrypt to another Mozilla-trusted authority. Embed a pinned, reproducible full CA bundle with provenance and regeneration checks.
- Enforce certificate validity dates before HTTP transmission, compensating for the pinned SDK being built without certificate-date verification. Keep standard chain and hostname verification and reject expired, wrong-host and untrusted certificates.
- Synchronize the clock after every boot, resume promptly after Wi-Fi reconnection, and use bounded retry delays with per-account errors and a retry countdown.
- Preserve retryability when token refresh fails before any HTTP request is sent; retain conservative reauthorization for ambiguous requests.
- Add USB connection diagnostics and positive/negative certificate tests. Remove obsolete Mac-bridge recovery text.

## 0.1.0-alpha.1

First public experimental release of QuotaPanel ESP32.

- Standalone direct usage and reset-credit polling for two accounts, with browser PKCE setup and on-device refresh.
- Touch dashboard with remaining allowance, credits, banked reset count/earliest expiry, reset timers and conditional runway forecasts.
- Persistent alerts for 10%, 5%, exhaustion, reset, inferred bank use and bank gain; redistributable offline speech and chimes.
- Portable USB tools, verified backup/flash/restore flow, pinned builds, tests, dependency notices and explicit hardware documentation.

Known limits: one tested board variant, two accounts required, internal endpoint dependency, unencrypted credential storage, no OTA, no long-duration reliability claim. See docs/RELEASE_READINESS.md.
