# Tested hardware

The original unit came from [this user-supplied AliExpress listing](https://www.aliexpress.us/item/3256810103745462.html). The listing could not be independently fetched during release preparation. The characteristics below were established from the working board, firmware and bench tests; they are not a promise about every seller option or future revision.

- ES3C28P-class / LCDWiki-style Xiaozhi 2.8-inch module.
- ESP32-S3, 16 MiB flash, 8 MiB octal PSRAM.
- ILI9341 320 × 240 landscape SPI display, inversion enabled.
- FT6336 capacitive touch; ES8311 codec and the board's two-pin speaker socket.
- 2.4 GHz Wi-Fi; USB data cable required for initial setup and recovery.

| Function | ESP32 GPIO / address |
| --- | --- |
| LCD CS / DC | 10 / 46 |
| LCD SCLK / MOSI / MISO | 12 / 11 / 13 |
| LCD backlight | 45 |
| LCD reset | Shared board EN; no separate GPIO |
| Touch + codec I²C SDA / SCL | 16 / 15 |
| FT6336 reset / interrupt / address | 18 / 17 / 0x38 |
| ES8311 address | 0x18 |
| Speaker amplifier enable | 1, active low |
| I²S MCLK / BCLK / LRCK / DAC data | 4 / 5 / 7 / 8 |
| BOOT button | 0 |
| Battery voltage divider (200k / 200k) | 9 (ADC1) |

With default rotation 3, touch coordinates are transformed to landscape as x = rawY, y = 239 − rawX. Audio is 22,050 Hz, 16-bit PCM over stereo I²S; voice clips are mono duplicated to the output channels. Connect the speaker to the board's speaker connector, not directly to a GPIO.

The board definition also identifies microphone data GPIO 6, RGB GPIO 42, and SD pins 38/40/39/41/48/47. These functions are not enabled or validated by this release. There is no microphone capture or audio upload.

## Battery display

Tap the power badge in the dashboard header to view BAT-pin voltage, voltage history and battery controls. Back returns to the dashboard; the speaker remains independently tappable. Usage alerts continue on the power page.

### Confirm battery-only operation

The charger can hold the empty BAT socket around 4.1 V. Alpha.5 incorrectly displayed that as a nearly full battery. Alpha.6 starts in **POWER UNCONFIRMED** (`BAT?`) and hides battery percentage, runtime and battery warnings. A high BAT reading does not establish that a cell is attached.

After connecting the battery and unplugging USB power, tap **START BATTERY RUN**, then confirm that the battery is powering the display. This is a user declaration, not automatic hardware detection. End the run when reconnecting USB, charging or changing/removing the cell. Firmware also cancels the run on a detected USB host, invalid/stale readings, a step over 100 mV or a rise over 40 mV above the run's minimum. These checks are conservative safeguards, not reliable detection of every power change: an ordinary wall charger or a cell removed near the charger voltage can be electrically indistinguishable. Each reboot requires a new confirmation.

### Battery warnings and runtime

During a confirmed battery-only run, voltage-derived charge at or below 15% for 60 seconds queues “Battery low. Please charge.” At or below 5% for 30 seconds queues the same voice with an urgent chime. Critical suppresses a duplicate low alert. The sound respects mute/volume, retries a full audio queue, and is discarded before playback if battery-only mode or the low level no longer applies. **TEST VOICE** plays a preview without changing warning latches. Latches persist in NVS and rearm only after a confirmed level of at least 25% for two minutes; repeated low readings, restarts and small rebounds do not repeatedly announce the warning.

Runtime means estimated time **to a 5% / 3.50 V reserve**, not an exact shutdown time. It learns from up to three hours of one-minute samples from the current confirmed run. A regression needs at least 15 minutes, 30 mV and three percentage points of decline, with a reasonably consistent trend (R-squared at least 0.6). A recent decline must still be observable. Eligible 30-minute and three-hour estimates are blended 70/30; insufficient, stale, rising or flat observations produce no estimate. A broad low-confidence sensitivity range accounts for rate disagreement and a 0.5–1.5x pace variation. Estimates above 24 hours are withheld. Load changes, temperature and the generic voltage curve still limit accuracy.

Runtime history intentionally restarts on reboot or power-mode changes so charging or unknown-power observations cannot contaminate a discharge estimate. Account runway persistence is independent and unchanged. Warning latches, unlike the runtime samples, survive reboot.

The [manufacturer schematic](https://www.lcdwiki.com/res/ES3C28P/2.8inch_ESP32-S3_Display_Schematic.pdf) connects BAT+ through two 200 kOhm resistors to GPIO 9. The TP4054 charger has no software interface, its CHRG signal is not routed to an ESP32 GPIO, and there is no current sensor or USB-voltage ADC connection. Charging status, power source, current, watts and USB input voltage are therefore unavailable. The charger rating (about 290 mA in the board specification) is not a live measurement. Battery voltage is the only measured power rail.

Firmware uses the pinned Arduino framework's calibrated millivolt conversion at 11 dB attenuation (the S3's lower attenuation ranges cannot cover BAT/2 at full charge). Every five seconds it discards the first conversion, trims 32 samples to the middle 16, doubles the mean for the divider, and smooths with an exponential filter. ADC calibration and resistor tolerances still limit absolute accuracy; compare with a multimeter for cell-specific validation.

Percentage during a confirmed run uses a generic 3.7 V / 4.2 V single-cell LiPo curve, rounded to 5% steps and marked with `~`. It is not a coulomb counter: load, cell chemistry, temperature and charging bias the estimate. It must not control charging or replace the battery's protection circuit. Unknown/out-of-range readings show `--`, not 0%.

Voltage trend is a least-squares fit over up to ten minutes of filtered samples, with at least three minutes before publishing a slope. Rising/falling labels require at least 2 mV/min and 10 mV total movement. These describe voltage only: load relaxation can look like charging. Invalid readings, sample gaps over 15 seconds and steps over 100 mV restart learning. This short battery history lives in RAM and restarts after reboot; persisted account runway history is independent.

The existing authenticated `wifi_update.py status` response includes BAT-pin voltage, user-confirmed battery-only state, estimated percentage/runtime, trend, observation coverage and audio counters. Percentage/runtime are null without confirmation; unsupported measurements and automatic presence detection are always JSON null. No new network endpoint or credential is introduced.

## Before flashing another board

Confirm the controller, pins, voltage requirements and flash/PSRAM configuration against its schematic. Chip and flash-size detection cannot identify a display controller or wiring variant. The PlatformIO board name supplies memory/build settings; it does not imply compatibility with the 4D Systems display itself.

Use a reliable USB supply and cable. Preserve a verified full-flash backup before replacing vendor firmware. If automatic reset fails, hold BOOT, tap RESET/EN, then release BOOT and retry with the newly enumerated USB port. This firmware does not burn eFuses, enable secure boot, or change flash-encryption state.
