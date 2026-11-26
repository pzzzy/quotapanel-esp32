# Setup and recovery

Follow the root README in order: install Python 3.11+, back up and flash, configure Wi-Fi, pair both accounts, then enable. Initial setup and reauthorization require USB. Once paired, firmware maintenance can use signed Wi-Fi updates; see [Wi-Fi updates](WIFI_UPDATES.md). Normal polling, TLS, refresh, forecasting, display updates and audio run on the ESP32.

## USB and Wi-Fi

Run `python tools/quotapanel.py ports`. Pass `--port` before the command, or set `QUOTAPANEL_PORT`. Auto-detection succeeds only when exactly one Espressif USB device is attached. Close other serial monitors before using a tool. Linux may need membership in the system's serial-device group; Windows uses a port such as COM5.

`configure` prompts for a 2.4 GHz SSID and password without echoing the password. Settings are stored on the board, not in a host configuration file. Enterprise Wi-Fi and captive portals are not supported. An empty password selects an open network.

Timezone strings use POSIX syntax, not IANA city names. Examples: `UTC0`; `EST5EDT,M3.2.0,M11.1.0` (US Eastern); `PST8PDT,M3.2.0,M11.1.0` (US Pacific). Reset and expiration epochs remain UTC internally. A clock sync is required before verified HTTPS begins.

## Account pairing

`pair --account 1` starts a short-lived loopback listener on 127.0.0.1:1455. Quit any other sign-in process using that port. The board creates PKCE/state, the browser performs login, and the ESP32 exchanges the returned one-time code directly for its own session. Physical security-key/passkey confirmation remains the user's action.

For account 2, run `pair --account 2` and choose your second account in the browser. Add `--no-browser` if you want to open the link in another browser profile. The firmware rejects pairing the same account into both slots and preserves the identity of an already paired slot. To change identities, use `forget` first. Avoid including authorization URLs, callbacks or account IDs in issue reports.

Run `enable` after both accounts are paired. The footer should become DIRECT. Browser setup is tested on macOS; Linux and Windows setup tools are designed to be portable but are not yet hardware-qualified.

## Recovery

- No data: run `diagnose` for per-account HTTP/TLS errors and retry state. A `TLS CERTIFICATE ERROR` needs an up-to-date firmware trust bundle; repeated sign-in does not fix it. Failed fetches retry automatically.
- No Wi-Fi: run `configure` again. Configuration works before Wi-Fi and clock setup.
- When the display shows **SIGN IN AGAIN**, connect USB and run `python tools/quotapanel.py reauth`. The command opens the affected sign-ins one at a time in your default browser and confirms live usage and reset credits. Choose the account shown by the setup tool at each prompt. Use `reauth --account 1` to target PRIMARY explicitly, or add `--no-browser` to open the links yourself.
- `diagnose` distinguishes an expired, revoked, reused or rejected refresh grant from an unknown refresh outcome. An interrupted request whose token may have rotated can still require browser sign-in. There is no fixed reauthorization interval imposed by QuotaPanel.
- Refresh test: `refresh --account 1`. Do not repeatedly force refresh in normal operation; the board refreshes proactively and after an authentication failure.
- Remove an account: `forget --account 1`, then type FORGET. This works offline, pauses polling and reboots. Pair both slots and enable to resume.
- After flashing: the tool pulses reset and verifies the application starts. If a status check still times out, tap RESET/EN once and wait for Wi-Fi/clock synchronization before retrying.
- USB timeout: close competing monitors, inspect cable/port, retry read-only diagnostics. Do not blindly replay token exchange or refresh operations after an uncertain result.
- Restore the original device: `python tools/flash.py --port YOUR_PORT restore PRIVATE_BACKUP_DIRECTORY`. Type RESTORE only when you intend to replace all flash. The tool verifies the saved image hash before writing and verifies the flashed result before boot.

Account removal is logical NVS deletion, not forensic erasure. Historical readings can remain in the separate journal. Before transferring ownership, revoke the device's account sessions and erase all flash using Espressif's erase-flash operation, then reinstall. A complete erase destroys the current application, credentials, Wi-Fi, history and any remaining vendor data. Keep any required backup privately first.

Never attach a flash backup, NVS dump, auth file, real DATA response or unredacted screenshot to a public report.
