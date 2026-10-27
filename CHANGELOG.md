# Changelog

## 0.1.0-alpha.1

First public experimental release of QuotaPanel ESP32.

- Standalone direct usage and reset-credit polling for two accounts, with browser PKCE setup and on-device refresh.
- Touch dashboard with remaining allowance, credits, banked reset count/earliest expiry, reset timers and conditional runway forecasts.
- Persistent alerts for 10%, 5%, exhaustion, reset, inferred bank use and bank gain; redistributable offline speech and chimes.
- Portable USB tools, verified backup/flash/restore flow, pinned builds, tests, dependency notices and explicit hardware documentation.

Known limits: one tested board variant, two accounts required, internal endpoint dependency, unencrypted credential storage, no OTA, no long-duration reliability claim. See docs/RELEASE_READINESS.md.
