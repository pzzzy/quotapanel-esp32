# Security policy

## Reporting

Report vulnerabilities privately through [GitHub private vulnerability reporting](https://github.com/pzzzy/quotapanel-esp32/security/advisories/new). Do not put live tokens, account IDs, screenshots with personal readings, authorization URLs, NVS images or full-flash backups in an issue. Use synthetic examples. Only the latest experimental alpha is maintained; there is no response-time guarantee.

## Threat model and limits

- The ESP32 holds two independent OAuth sessions and a Wi-Fi password in **unencrypted flash**. Physical/USB access or a firmware compromise can expose them. This alpha does not enable secure boot or flash encryption.
- USB setup is a trusted administrative interface. Anyone with access can read diagnostics and modify configuration. There is no network configuration server or runtime host bridge.
- HTTPS verifies certificates and hostnames. A fresh clock sync after every boot and maintenance of the bundled Mozilla/certifi trust store are required. Do not disable verification to work around network failures.
- The host's browser performs initial authentication; PKCE/state protect the code exchange. The loopback receiver binds only 127.0.0.1, checks Host and state, and does not log callback URLs. A trusted setup computer is still necessary.
- Pending refresh state is committed before rotation. An uncertain rotation requires fresh login. A failure proven to occur before HTTP transmission can retry after clearing and verifying the pending marker. Token values are not intentionally returned in diagnostic reports, but treat all account diagnostics as private.
- 'Forget' deletes NVS keys logically; flash wear-leveling and history may retain old bytes. Revoke sessions and erase all flash before transferring ownership.
- Full backups contain device data and may contain tokens. Tools create backup directories with mode 0700 and images with mode 0600 where supported. Filesystem ACLs and cloud backups remain your responsibility.
- Internal OpenAI endpoints and the native client flow are not a supported third-party API guarantee. This project cannot guarantee future availability or eligibility for every account.

The firmware reads account status. It does not automatically redeem banked resets, purchase credits, send prompts or capture microphone input. Data freshness and forecast uncertainty are visible product limitations, not grounds to relax authentication or TLS checks.
