#pragma once

// All five devices join this 2.4 GHz Wi-Fi network. Set the password locally
// over USB with tools/set_wifi_password.py; it is stored on each ESP32 in NVS.
#define HOUSE_WIFI_SSID "YOUR_WIFI_SSID"

// Change this six-digit code before installation. BLE control commands start
// with the code, for example: CHANGE_ME TIMER 600
#define HOUSE_CONTROL_CODE "CHANGE_ME"

// Default playback level for speech and alarm on every box.
#define HOUSE_VOLUME_PERCENT 80

// Night mode: beeps, the double-tap melody and the timer alarm play at this
// share of the normal volume; speech keeps its volume. Times are local minutes since
// midnight; the clock comes from the internet (NTP) through the router.
#define HOUSE_TIMEZONE "MSK-3"
#define HOUSE_NIGHT_START_MIN (23 * 60)      // 23:00
#define HOUSE_NIGHT_END_MIN (7 * 60 + 30)    // 07:30
#define HOUSE_NIGHT_VOLUME_SHARE 50          // percent of normal volume
#define HOUSE_LED_DAY_PERCENT 25             // status LED brightness by day
#define HOUSE_LED_NIGHT_PERCENT 10           // ... and at night (23:00-07:30)

// BLE control forces Wi-Fi modem sleep (ESP32 coexistence). In modem sleep the
// router holds frames until its DTIM beacon and releases them in bursts, which
// made voice between boxes stutter. Default: BLE off, Wi-Fi always awake,
// timer/stop/volume through the box web page (http://<box IP>/).
#ifndef HOUSE_ENABLE_BLE
#define HOUSE_ENABLE_BLE 0
#endif

// Optional bare 1602A screen on one station, without an I2C backpack.
// Logic VDD must be 3V3, RW and VO must be GND; see README before wiring.
// The same image also runs normally on stations without an LCD attached.
#ifndef HOUSE_LCD1602_DIRECT
#define HOUSE_LCD1602_DIRECT 1
#endif
