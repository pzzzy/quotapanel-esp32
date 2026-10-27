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

Touch coordinates are transformed to landscape as x = 319 − rawY, y = rawX. Audio is 22,050 Hz, 16-bit PCM over stereo I²S; voice clips are mono duplicated to the output channels. Connect the speaker to the board's speaker connector, not directly to a GPIO.

The board definition also identifies microphone data GPIO 6, RGB GPIO 42, battery ADC GPIO 9, and SD pins 38/40/39/41/48/47. These functions are not enabled or validated by this release. There is no microphone capture or audio upload.

## Before flashing another board

Confirm the controller, pins, voltage requirements and flash/PSRAM configuration against its schematic. Chip and flash-size detection cannot identify a display controller or wiring variant. The PlatformIO board name supplies memory/build settings; it does not imply compatibility with the 4D Systems display itself.

Use a reliable USB supply and cable. Preserve a verified full-flash backup before replacing vendor firmware. If automatic reset fails, hold BOOT, tap RESET/EN, then release BOOT and retry with the newly enumerated USB port. This firmware does not burn eFuses, enable secure boot, or change flash-encryption state.
