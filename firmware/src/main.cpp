#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <lwip/sockets.h>
#include "config.h"
#if HOUSE_ENABLE_BLE
#include <NimBLEDevice.h>
#endif
#include <driver/i2s.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <Update.h>
#include <math.h>
#if HOUSE_LCD1602_DIRECT
#include "lcd1602_direct.h"
#endif

// One firmware image is used in all five boxes. Its BLE name is derived
// from the unique ESP32 MAC address; no per-box recompilation is required.
// Audio on the house Wi-Fi is 16 kHz, 16-bit, mono, 20 ms per UDP packet.

namespace {
constexpr int SAMPLE_RATE = 16000;
constexpr int FRAME_SAMPLES = 320;
constexpr int FRAME_BYTES = FRAME_SAMPLES * 2;
constexpr uint32_t SHORT_TAP_MS = 280;
constexpr uint32_t DOUBLE_TAP_GAP_MS = 500;
constexpr uint32_t MAX_TALK_MS = 60000;
constexpr uint8_t ASSISTANT_ROOM = 6;         // voice assistant gateway on the server
constexpr uint32_t ASSIST_SILENCE_MS = 900;   // pause that ends a hands-free question
constexpr uint32_t ASSIST_NO_SPEECH_MS = 5000;
constexpr uint32_t ASSIST_MAX_MS = 12000;
constexpr uint32_t ASSIST_SKIP_MS = 220;      // do not send our own start beep
constexpr uint32_t CHIME_FRAMES = 60; // three notes, 1.2 seconds total
constexpr uint16_t UDP_PORT = 42042;
constexpr uint32_t MAGIC = 0x354d4f48; // HOM5, little-endian
constexpr int DEFAULT_ALL_BUTTON_PIN = 32;
constexpr int ROOM2_ALL_BUTTON_PIN = 23; // room 2: moved from D32 to upper-right D23
// Physical buttons from top to bottom on the first box, confirmed over USB.
constexpr int PRIVATE_BUTTON_PINS[4] = {18, 19, 21, 33};
constexpr int STATUS_LED_PIN = 2; // active high; series resistor and LED to GND
constexpr int MIC_BCLK = 14;
constexpr int MIC_WS = 27;
constexpr int DEFAULT_MIC_DATA = 34; // the 3F-A box has DOUT on D35: MIC PIN 35
constexpr int AMP_BCLK = 26;
constexpr int AMP_WS = 25;
constexpr int DEFAULT_AMP_DATA = 22; // next to TX0; screenless boxes may move it
// Amplifier DIN pins allowed by `AMP PIN n`: free, non-strapping outputs.
constexpr int AMP_DATA_CHOICES[] = {22, 16, 17, 4, 13};
constexpr int QUEUE_FRAMES = 24;
constexpr int START_BUFFER_FRAMES = 6; // 120 ms to absorb Wi-Fi scheduling jitter

const char *SERVICE_UUID = "77a5d8a0-7bb2-4b6a-9bf9-3336a781db11";
const char *COMMAND_UUID = "77a5d8a1-7bb2-4b6a-9bf9-3336a781db11";
const char *STATUS_UUID  = "77a5d8a2-7bb2-4b6a-9bf9-3336a781db11";

// Every box broadcasts HELLO with its room number; voice then goes as unicast
// to known boxes. Unicast is acknowledged and retried by Wi-Fi at full rate;
// broadcast is sent once at the slowest rate and lost silently between floors.
constexpr uint32_t HELLO_INTERVAL_MS = 2000;
constexpr uint32_t PEER_TIMEOUT_MS = 15000;

const char *FW_VERSION = "2026-10-24";
constexpr int WATCHDOG_TIMEOUT_S = 30;          // a hung task reboots the box
// One reboot per outage after 10 min without Wi-Fi; a router switched off for
// the night must not make the boxes restart (and click) every few minutes.
constexpr uint32_t WIFI_REBOOT_MS = 10 * 60 * 1000;
constexpr uint32_t WIFI_REBOOT_MAGIC = 0x57494649; // "WIFI"
RTC_NOINIT_ATTR uint32_t wifiRebootMarker;         // survives a software restart
constexpr uint32_t CUE_QUIET_MS = 3000;          // cue tone if nothing was heard for 3 s

enum LocalTone : uint8_t { TONE_NONE = 0, TONE_CUE = 1, TONE_UNAVAILABLE = 2, TONE_SENT = 3 };

enum PacketType : uint8_t { VOICE = 1, TIMER = 3, STOP = 4, VOLUME = 5, HELLO = 6, ASSIST = 7 };
// ASSIST packets go to the assistant only: the question is over, or cancelled.
enum AssistSignal : uint32_t { ASSIST_DONE = 1, ASSIST_CANCEL = 2 };
struct __attribute__((packed)) PacketHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t sequence;
  uint32_t origin;
  uint32_t value; // VOICE: destination room (0 = all); control: command value
};
static_assert(sizeof(PacketHeader) == 16, "wire header size");

struct Peer { uint32_t ip; uint32_t node; uint32_t seenMs; }; // ip in network order

int rxSock = -1; // owned by netTask
int txSock = -1; // guarded by udpTxMutex
uint32_t broadcastNet = 0;
Peer peers[ASSISTANT_ROOM + 1] = {}; // index = room 1..5, 6 = assistant
WebServer web(80);
bool webStarted = false;
#if HOUSE_ENABLE_BLE
NimBLECharacteristic *statusCharacteristic = nullptr;
#endif
SemaphoreHandle_t udpTxMutex = nullptr;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE peerMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t nodeId = 0;
volatile uint8_t roomNumber = 0; // 1..5; zero means not yet assigned
int allButtonPin = DEFAULT_ALL_BUTTON_PIN;
int ampDataPin = DEFAULT_AMP_DATA;
int micDataPin = DEFAULT_MIC_DATA;
int assistPin = -1;  // optional dedicated assistant button, set with ASSIST PIN n
char deviceName[20] = {};
IPAddress boundIp;
volatile bool udpReady = false;
String wifiPassword;
uint16_t sequenceNo = 0;
uint32_t rxPackets = 0;
uint32_t rxValid = 0;
uint32_t rxVoiceAccepted = 0;
uint32_t rxVoiceBlocked = 0;
uint32_t rxVoiceOtherRoom = 0;
uint32_t rxControls = 0;
uint32_t rxVoiceSeqGaps = 0;
uint32_t rxVoiceReordered = 0;
uint32_t rxMaxInterpacketMs = 0;
uint32_t lastVoiceOrigin = 0;
uint16_t lastVoiceSequence = 0;
uint32_t lastVoiceArrivalMs = 0;
volatile uint32_t txSent = 0;
volatile uint32_t txFailed = 0;
volatile uint32_t txBusy = 0;
volatile uint32_t txMaxSendUs = 0;
volatile uint32_t txSlow = 0;
volatile uint32_t micReadFull = 0;
volatile uint32_t micReadShort = 0;
volatile uint32_t captureLateTicks = 0;
volatile uint32_t captureMaxPeriodMs = 0;
volatile bool alarmActive = false;
uint64_t deadlineMs = 0;
uint32_t soundTestUntilMs = 0;
volatile uint32_t micTestUntilMs = 0;
volatile int volumePercent = HOUSE_VOLUME_PERCENT;
volatile uint32_t remoteVoiceLastMs = 0;
volatile uint32_t lastAssistantVoiceMs = 0;  // last answer frame from the assistant
volatile bool assistMuted = false;  // answer cancelled by a button: drop it until the next question
volatile bool localTalking = false;
volatile uint8_t activeTarget = 255;
volatile uint8_t usbChimeTarget = 255; // TEST CHIME over USB, 0 = all
volatile bool usbAssistRequest = false; // TEST ASSIST over USB
volatile uint8_t toneRequest = TONE_NONE; // local tone for playbackTask
volatile bool nightMode = false;          // refreshed by loop() from the NTP clock
// Status LED brightness, percent of perceived brightness; `LED day night` changes them.
uint8_t ledDayPercent = HOUSE_LED_DAY_PERCENT;
uint8_t ledNightPercent = HOUSE_LED_NIGHT_PERCENT;
constexpr uint8_t LED_CHANNEL = 0;        // LEDC PWM channel for the status LED
volatile uint8_t deniedTarget = 255;      // last room found offline (LCD hint)
volatile uint32_t deniedAtMs = 0;
bool otaAuthorized = false;
bool otaWritten = false;
uint32_t lastWifiAttemptMs = 0;
#if HOUSE_LCD1602_DIRECT
Lcd1602Direct lcd;
#endif

struct ControlPending { uint8_t type; uint32_t value; };
ControlPending pending = {0, 0};
int16_t audioQueue[QUEUE_FRAMES][FRAME_SAMPLES];
uint8_t queueRead = 0, queueWrite = 0, queueCount = 0;
bool queuePrimed = false;
volatile uint32_t queueUnderruns = 0;
volatile uint32_t queueOverruns = 0;
volatile uint8_t queueMaxDepth = 0;

void clearQueue() {
  portENTER_CRITICAL(&queueMux);
  queueRead = queueWrite = queueCount = 0;
  queuePrimed = false;
  portEXIT_CRITICAL(&queueMux);
}

void pushFrame(const int16_t *frame) {
  portENTER_CRITICAL(&queueMux);
  if (queueCount == QUEUE_FRAMES) {
    queueRead = (queueRead + 1) % QUEUE_FRAMES; // keep current sound, not stale sound
    --queueCount;
    ++queueOverruns;
  }
  memcpy(audioQueue[queueWrite], frame, FRAME_BYTES);
  queueWrite = (queueWrite + 1) % QUEUE_FRAMES;
  ++queueCount;
  if (queueCount > queueMaxDepth) queueMaxDepth = queueCount;
  portEXIT_CRITICAL(&queueMux);
}

bool playbackIdle() {
  portENTER_CRITICAL(&queueMux);
  bool idle = queueCount == 0;
  portEXIT_CRITICAL(&queueMux);
  return idle;
}

bool popFrame(int16_t *frame) {
  bool result = false;
  portENTER_CRITICAL(&queueMux);
  if (!queuePrimed && queueCount >= START_BUFFER_FRAMES) queuePrimed = true;
  if (queuePrimed && !queueCount) {
    ++queueUnderruns;
    queuePrimed = false;
  }
  if (queuePrimed && queueCount) {
    memcpy(frame, audioQueue[queueRead], FRAME_BYTES);
    queueRead = (queueRead + 1) % QUEUE_FRAMES;
    --queueCount;
    result = true;
  }
  portEXIT_CRITICAL(&queueMux);
  return result;
}

int16_t clip16(int32_t x) {
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return static_cast<int16_t>(x);
}

void fillTestChimeFrame(int16_t *frame, uint32_t frameIndex) {
  // A fixed three-note test melody sent as ordinary VOICE packets. It works
  // even when the microphone or the LittleFS alarm file is unavailable.
  constexpr int noteHz[3] = {523, 659, 784};
  for (int i = 0; i < FRAME_SAMPLES; ++i) {
    uint32_t sampleIndex = frameIndex * FRAME_SAMPLES + i;
    uint32_t note = sampleIndex / 6400; // 400 ms per note
    uint32_t withinNote = sampleIndex % 6400;
    if (note >= 3 || withinNote >= 4800) { // 300 ms note, 100 ms gap
      frame[i] = 0;
      continue;
    }
    uint32_t fade = withinNote < 4799 - withinNote ? withinNote : 4799 - withinNote;
    if (fade > 160) fade = 160;
    float phase = 2.0f * PI * noteHz[note] * withinNote / SAMPLE_RATE;
    frame[i] = int16_t(11000.0f * (fade / 160.0f) * sinf(phase));
  }
}

// Local tones played only by this box. Returns false after the last frame.
// Cue: two short rising beeps before incoming speech (180 ms).
// Unavailable: two falling low beeps when the called box is offline (360 ms).
// Sent: two short falling beeps when a question went to the assistant (180 ms).
bool fillToneFrame(uint8_t tone, uint32_t frameIndex, int16_t *frame) {
  int hz = 0, amplitude = 0;
  if (tone == TONE_CUE) {
    if (frameIndex >= 9) return false;
    hz = frameIndex < 4 ? 880 : frameIndex >= 5 ? 1320 : 0;
    amplitude = 7000;
  } else if (tone == TONE_SENT) {
    if (frameIndex >= 9) return false;
    hz = frameIndex < 4 ? 1320 : frameIndex >= 5 ? 880 : 0;
    amplitude = 7000;
  } else if (tone == TONE_UNAVAILABLE) {
    if (frameIndex >= 18) return false;
    hz = frameIndex < 8 ? 440 : frameIndex >= 10 ? 330 : 0;
    amplitude = 9000;
  } else {
    return false;
  }
  for (int i = 0; i < FRAME_SAMPLES; ++i) {
    uint32_t sampleIndex = frameIndex * FRAME_SAMPLES + i;
    frame[i] = hz ? int16_t(amplitude * sinf(2.0f * PI * hz * sampleIndex / SAMPLE_RATE)) : 0;
  }
  return true;
}

void setTimerSeconds(uint32_t seconds) {
  portENTER_CRITICAL(&stateMux);
  alarmActive = false;
  deadlineMs = esp_timer_get_time() / 1000ULL + uint64_t(seconds) * 1000ULL;
  portEXIT_CRITICAL(&stateMux);
  clearQueue();
}

bool saveRoomNumber(uint8_t room) {
  if (room < 1 || room > 5) return false;
  Preferences prefs;
  if (!prefs.begin("house", false)) return false;
  bool saved = prefs.putUChar("room", room) == 1;
  prefs.end();
  if (saved) roomNumber = room;
  return saved;
}

const char *roomShortName(uint8_t room) {
  switch (room) {
    case 1: return "1F";       // first floor
    case 2: return "2F";       // second floor
    case 3: return "3F-A";     // third floor, room 1
    case 4: return "3F-B";     // third floor, room 2
    case 5: return "4F";       // fourth floor
    case 6: return "AI";       // voice assistant
    default: return "R0";
  }
}

uint8_t privateTargetForButton(uint8_t button) {
  uint8_t own = roomNumber;
  if (own < 1 || own > 5 || button > 3) return 255;
  uint8_t index = 0;
  for (uint8_t room = 1; room <= 5; ++room) {
    if (room == own) continue;
    if (index++ == button) return room;
  }
  return 255;
}

void stopAlarm() {
  portENTER_CRITICAL(&stateMux);
  alarmActive = false;
  deadlineMs = 0;
  portEXIT_CRITICAL(&stateMux);
  clearQueue();
}

void scheduleControl(uint8_t type, uint32_t value) {
  portENTER_CRITICAL(&stateMux);
  pending = {type, value};
  portEXIT_CRITICAL(&stateMux);
}

String statusText() {
  uint64_t deadline;
  bool ringing;
  portENTER_CRITICAL(&stateMux);
  deadline = deadlineMs;
  ringing = alarmActive;
  portEXIT_CRITICAL(&stateMux);
  String prefix = "ROOM " + String(roomNumber) + " ";
  if (ringing) return prefix + "RINGING";
  if (!deadline) return prefix + "IDLE";
  uint64_t now = esp_timer_get_time() / 1000ULL;
  uint32_t remaining = deadline > now ? (deadline - now + 999) / 1000 : 0;
  return prefix + "TIMER " + String(remaining) + "s";
}

uint8_t readButtonMask() {
  uint8_t mask = digitalRead(allButtonPin) == LOW ? 1 : 0;
  for (uint8_t i = 0; i < 4; ++i) {
    if (digitalRead(PRIVATE_BUTTON_PINS[i]) == LOW) mask |= uint8_t(1U << (i + 1));
  }
  return mask;
}

void printButtonMask(uint8_t mask) {
  Serial.printf("BUTTONS ALL=%u K1=%u K2=%u K3=%u K4=%u\n",
                unsigned((mask >> 0) & 1), unsigned((mask >> 1) & 1),
                unsigned((mask >> 2) & 1), unsigned((mask >> 3) & 1),
                unsigned((mask >> 4) & 1));
}

void monitorButtons() {
  // Report only states stable for 50 ms, so switch bounce does not flood USB.
  static uint8_t candidate = 0xff;
  static uint8_t reported = 0xff;
  static uint32_t candidateSince = 0;
  uint8_t current = readButtonMask();
  if (current != candidate) {
    candidate = current;
    candidateSince = millis();
  }
  if (candidate != reported && millis() - candidateSince >= 50) {
    reported = candidate;
    printButtonMask(reported);
  }
}

#if HOUSE_LCD1602_DIRECT
const char *roomLongName(uint8_t room) {
  switch (room) {
    case 0: return "ALL ROOMS";
    case 1: return "FLOOR 1";
    case 2: return "FLOOR 2";
    case 3: return "DANYA";      // third floor, room 1 (K2 on the first box)
    case 4: return "KRISTINA";   // third floor, room 2 (K3 on the first box)
    case 5: return "FLOOR 4";
    case ASSISTANT_ROOM: return "ASSISTANT";
    default: return "";
  }
}

uint8_t roomOfNode(uint32_t node) {
  uint8_t room = 0;
  portENTER_CRITICAL(&peerMux);
  for (uint8_t r = 1; r <= ASSISTANT_ROOM; ++r)
    if (peers[r].ip && peers[r].node == node) room = r;
  portEXIT_CRITICAL(&peerMux);
  return room;
}

void centerText(char (&out)[17], const char *text) {
  size_t length = strlen(text);
  if (length > 16) length = 16;
  size_t left = (16 - length) / 2;
  memset(out, ' ', 16);
  memcpy(out + left, text, length);
  out[16] = '\0';
}

void updateLcd() {
  // Only the first room has an LCD. On room 2, GPIO23 is the ALL button.
  if (roomNumber != 1) return;
  static uint32_t lastRefreshMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastRefreshMs < 400) return;
  lastRefreshMs = nowMs;

  char top[17] = {};
  char bottom[17] = {};
  bool networkReady = udpReady && WiFi.status() == WL_CONNECTED;
  snprintf(top, sizeof(top), "%s %s", roomShortName(roomNumber),
           networkReady ? "READY" : "NO WIFI");

  uint64_t deadline;
  bool ringing;
  portENTER_CRITICAL(&stateMux);
  deadline = deadlineMs;
  ringing = alarmActive;
  portEXIT_CRITICAL(&stateMux);

  // The first unit's LCD was installed upside down and cannot be turned
  // without desoldering. Each screen below needs at most eight drawn letters
  // besides the rotation-proof ROM ones (O H I N S X Z 0 8 : - M W 6 9).
  if (roomNumber == 1) {
    char upper[17] = {};
    char lower[17] = {};
    if (ringing) {
      snprintf(upper, sizeof(upper), "ALARM");
      snprintf(lower, sizeof(lower), "STOP ON PHONE");
    } else if (localTalking) {
      snprintf(upper, sizeof(upper), "TALK TO");
      snprintf(lower, sizeof(lower), "%s", roomLongName(activeTarget));
    } else if (nowMs - remoteVoiceLastMs < 1500) {
      uint8_t caller = roomOfNode(lastVoiceOrigin);
      snprintf(upper, sizeof(upper), "CALL FROM");
      snprintf(lower, sizeof(lower), "%s", caller ? roomLongName(caller) : "");
    } else if (deniedTarget != 255 && nowMs - deniedAtMs < 2000) {
      snprintf(upper, sizeof(upper), "%s", roomLongName(deniedTarget));
      snprintf(lower, sizeof(lower), "OFFLINE");
    } else if (deadline) {
      uint64_t now = esp_timer_get_time() / 1000ULL;
      uint32_t seconds = deadline > now ? uint32_t((deadline - now + 999) / 1000) : 0;
      snprintf(upper, sizeof(upper), "TIMER");
      if (seconds >= 3600)
        snprintf(lower, sizeof(lower), "%luH %02luM", (unsigned long)(seconds / 3600),
                 (unsigned long)((seconds / 60) % 60));
      else
        snprintf(lower, sizeof(lower), "%02lu:%02lu", (unsigned long)(seconds / 60),
                 (unsigned long)(seconds % 60));
    } else {
      // Idle: K1..K4 targets on the upper line, next to the buttons above the
      // screen; status below, left-aligned. "OK" instead of "READY" keeps this
      // screen within the eight drawn letters.
      char idle[17], keys[17];
      snprintf(idle, sizeof(idle), "FLOOR 1 %s", networkReady ? "OK" : "NO WIFI");
      snprintf(keys, sizeof(keys), "2   D   K   4");
      lcd.showUpsideDown(keys, idle);
      return;
    }
    char upperCentered[17], lowerCentered[17];
    centerText(upperCentered, upper);
    centerText(lowerCentered, lower);
    lcd.showUpsideDown(upperCentered, lowerCentered);
    return;
  }

  if (ringing) {
    snprintf(bottom, sizeof(bottom), "ALARM: BLE STOP");
  } else if (localTalking) {
    if (activeTarget == 0) snprintf(bottom, sizeof(bottom), "TO ALL ROOMS");
    else snprintf(bottom, sizeof(bottom), "TO %s", roomShortName(activeTarget));
  } else if (nowMs - remoteVoiceLastMs < 350) {
    snprintf(bottom, sizeof(bottom), "VOICE INCOMING");
  } else if (deadline) {
    uint64_t now = esp_timer_get_time() / 1000ULL;
    uint32_t seconds = deadline > now ? uint32_t((deadline - now + 999) / 1000) : 0;
    snprintf(bottom, sizeof(bottom), "TIMER %02lu:%02lu:%02lu",
             (unsigned long)(seconds / 3600),
             (unsigned long)((seconds / 60) % 60),
             (unsigned long)(seconds % 60));
  } else {
    snprintf(bottom, sizeof(bottom), "%s", networkReady ? "PRESS TO TALK" : "WAIT FOR WIFI");
  }
  lcd.show(top, bottom);
}
#endif

bool sendRaw(uint32_t ipNet, const uint8_t *data, size_t length);
String peersText();

bool statsPeriodic = false;

String statsText() {
  char text[700];
  snprintf(text, sizeof(text),
           "RXSTAT wifi=%d rssi=%d udp=%u heap=%lu min=%lu maxblock=%lu packets=%lu valid=%lu voice=%lu blocked=%lu other=%lu control=%lu\n"
           "AUDIOSTAT seq_gaps=%lu reorder=%lu max_gap_ms=%lu q_max=%u q_underrun=%lu q_overrun=%lu tx_ok=%lu tx_fail=%lu tx_busy=%lu tx_slow=%lu tx_max_us=%lu mic_full=%lu mic_short=%lu capture_late=%lu capture_max_ms=%lu\n",
           int(WiFi.status()), WiFi.RSSI(), unsigned(udpReady),
           (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)ESP.getMaxAllocHeap(),
           (unsigned long)rxPackets, (unsigned long)rxValid,
           (unsigned long)rxVoiceAccepted, (unsigned long)rxVoiceBlocked,
           (unsigned long)rxVoiceOtherRoom, (unsigned long)rxControls,
           (unsigned long)rxVoiceSeqGaps, (unsigned long)rxVoiceReordered,
           (unsigned long)rxMaxInterpacketMs, unsigned(queueMaxDepth),
           (unsigned long)queueUnderruns, (unsigned long)queueOverruns,
           (unsigned long)txSent, (unsigned long)txFailed,
           (unsigned long)txBusy, (unsigned long)txSlow,
           (unsigned long)txMaxSendUs, (unsigned long)micReadFull,
           (unsigned long)micReadShort,
           (unsigned long)captureLateTicks,
           (unsigned long)captureMaxPeriodMs);
  return String(text) + "PEERS " + peersText() + "\n";
}

void processUsbSetup() {
  static char line[72] = {};
  static uint8_t used = 0;
  while (Serial.available()) {
    char c = char(Serial.read());
    if (c == '\r' || c == '\n') {
      if (!used) continue;
      line[used] = '\0';
      if (strncmp(line, "ROOM ", 5) == 0 &&
          line[5] >= '1' && line[5] <= '5' && line[6] == '\0') {
        uint8_t room = uint8_t(line[5] - '0');
        Serial.println(saveRoomNumber(room) ? "ROOM SAVED; RESET FOR BLE NAME" :
                                             "ERROR: ROOM SAVE FAILED");
      } else if (strcmp(line, "STATUS") == 0) {
        Serial.printf("%s %s\n", deviceName, statusText().c_str());
      } else if (strcmp(line, "TEST UDP") == 0) {
        if (!udpReady || WiFi.status() != WL_CONNECTED || !udpTxMutex) {
          Serial.println("TEST UDP unavailable: Wi-Fi or UDP not ready");
        } else if (xSemaphoreTake(udpTxMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
          Serial.println("TEST UDP unavailable: transmit busy");
        } else {
          PacketHeader probe{MAGIC, 2, VOLUME, 0, nodeId, uint32_t(volumePercent)};
          bool sent = txSock >= 0 &&
                      sendRaw(uint32_t(WiFi.localIP()),
                              reinterpret_cast<const uint8_t *>(&probe), sizeof(probe));
          xSemaphoreGive(udpTxMutex);
          Serial.printf("TEST UDP self-send=%u to %s:%u\n", unsigned(sent),
                        WiFi.localIP().toString().c_str(), unsigned(UDP_PORT));
        }
      } else if (strncmp(line, "TEST CHIME ", 11) == 0 &&
                 line[11] >= '0' && line[11] <= '5' && line[12] == '\0') {
        usbChimeTarget = uint8_t(line[11] - '0');
        Serial.printf("TEST CHIME queued for room %c (0 = all)\n", line[11]);
      } else if (strcmp(line, "TEST ASSIST") == 0) {
        usbAssistRequest = true;
        Serial.println("TEST ASSIST queued: speak after the beep");
      } else if (strncmp(line, "ASSIST PIN ", 11) == 0) {
        int pin = atoi(line + 11);
        // 0 disables; otherwise a free GPIO with a pull-up. On the LCD box only
        // GPIO15 is free (4, 5, 13, 16, 17 drive the screen).
        bool allowed = pin == 0 || pin == 4 || pin == 5 || pin == 13 || pin == 15 ||
                       pin == 16 || pin == 17;
#if HOUSE_LCD1602_DIRECT
        allowed = allowed && (pin == 0 || pin == 15 || roomNumber != 1);
#endif
        Preferences prefs;
        if (!allowed) {
          Serial.println("ERROR: ASSIST PIN must be 0 (off), 4, 5, 13, 15, 16 or 17 (LCD box: 15)");
        } else if (prefs.begin("house", false) && prefs.putUChar("assist_pin", uint8_t(pin)) == 1) {
          prefs.end();
          Serial.printf("ASSIST PIN %d SAVED; RESET TO APPLY\n", pin);
        } else {
          Serial.println("ERROR: ASSIST PIN save failed");
        }
      } else if (strncmp(line, "MIC PIN ", 8) == 0) {
        // Microphone DOUT: input-only D34 (default) or its neighbour D35.
        int pin = atoi(line + 8);
        Preferences prefs;
        if (pin != 34 && pin != 35) {
          Serial.println("ERROR: MIC PIN must be 34 or 35");
        } else if (prefs.begin("house", false) && prefs.putUChar("mic_dout", uint8_t(pin)) == 1) {
          prefs.end();
          Serial.printf("MIC PIN %d SAVED; RESET TO APPLY\n", pin);
        } else {
          Serial.println("ERROR: MIC PIN save failed");
        }
      } else if (strncmp(line, "AMP PIN ", 8) == 0) {
        int pin = atoi(line + 8);
        bool allowed = false;
        for (int choice : AMP_DATA_CHOICES) allowed = allowed || choice == pin;
#if HOUSE_LCD1602_DIRECT
        // On the LCD box these pins drive the screen.
        allowed = allowed && (pin == 22 || roomNumber != 1);
#endif
        Preferences prefs;
        if (!allowed) {
          Serial.println("ERROR: AMP PIN must be 22, 16, 17, 4 or 13 (LCD box: 22 only)");
        } else if (prefs.begin("house", false) && prefs.putUChar("amp_din", uint8_t(pin)) == 1) {
          prefs.end();
          Serial.printf("AMP PIN %d SAVED; RESET TO APPLY\n", pin);
        } else {
          Serial.println("ERROR: AMP PIN save failed");
        }
      } else if (strcmp(line, "PEERS") == 0) {
        Serial.printf("PEERS %s\n", peersText().c_str());
      } else if (strcmp(line, "STATS") == 0) {
        Serial.print(statsText());
      } else if (strcmp(line, "STATS ON") == 0 || strcmp(line, "STATS OFF") == 0) {
        statsPeriodic = line[7] == 'N';
        Serial.printf("STATS every 5 s %s\n", statsPeriodic ? "ON" : "OFF");
      } else if (strcmp(line, "BUTTONS") == 0) {
        printButtonMask(readButtonMask());
      } else if (strcmp(line, "TEST SOUND") == 0) {
        soundTestUntilMs = millis() + 5000;
        portENTER_CRITICAL(&stateMux);
        deadlineMs = 0;
        alarmActive = true;
        portEXIT_CRITICAL(&stateMux);
        clearQueue();
        Serial.println("TEST SOUND START: local alarm for 5 seconds");
      } else if (strcmp(line, "TEST MIC") == 0) {
        micTestUntilMs = millis() + 10000;
        Serial.println("TEST MIC START: speak near microphone for 10 seconds");
      } else if (strcmp(line, "STOP") == 0) {
        soundTestUntilMs = 0;
        micTestUntilMs = 0;
        stopAlarm();
        Serial.println("TESTS STOPPED");
      } else if (strncmp(line, "VOLUME ", 7) == 0) {
        int amount = atoi(line + 7);
        if (amount < 1 || amount > 80) {
          Serial.println("ERROR: VOLUME must be 1..80");
        } else {
          volumePercent = amount;
          Serial.printf("VOLUME %d (this box only)\n", amount);
        }
      } else if (strncmp(line, "WIFI ", 5) == 0) {
        const char *candidate = line + 5;
        size_t length = strlen(candidate);
        bool valid = length >= 8 && length <= 63;
        for (size_t i = 0; valid && i < length; ++i)
          valid = uint8_t(candidate[i]) >= 32 && uint8_t(candidate[i]) <= 126;
        if (!valid) {
          Serial.println("ERROR: Wi-Fi password needs 8..63 printable ASCII characters");
        } else {
          Preferences prefs;
          if (prefs.begin("house", false)) {
            size_t written = prefs.putString("wifi_pwd", candidate);
            prefs.end();
            if (written) {
              wifiPassword = candidate;
              WiFi.disconnect(); // netTask closes the sockets when the link drops
              lastWifiAttemptMs = millis() - 5001;
              Serial.println("WIFI SAVED; connecting to configured SSID");
            } else {
              Serial.println("ERROR: Wi-Fi password save failed");
            }
          } else {
            Serial.println("ERROR: settings unavailable");
          }
        }
      } else {
        Serial.println("Use ROOM 1..5, STATUS, STATS, STATS ON/OFF, PEERS, BUTTONS, VOLUME 1..80, TEST SOUND, TEST MIC, TEST CHIME 0..5, STOP, or WIFI <password>");
      }
      used = 0;
    } else if (used < sizeof(line) - 1) {
      line[used++] = c;
    } else {
      used = 0;
      Serial.println("ERROR: command too long");
    }
  }
}

void updateBleStatus(const String &message) {
#if HOUSE_ENABLE_BLE
  if (!statusCharacteristic) return;
  statusCharacteristic->setValue(message.c_str());
  statusCharacteristic->notify();
#else
  (void)message;
#endif
}

// Shared by the web page and BLE. `cmd` is already without the control code.
String runCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.startsWith("ROOM ")) {
    int room = cmd.substring(5).toInt();
    if (room < 1 || room > 5) return "ERR ROOM 1..5";
    if (!saveRoomNumber(uint8_t(room))) return "ERR ROOM SAVE";
    return "ROOM " + String(room) + " SAVED; RESET";
  }
  // TIMER / STOP act on every box; the _LOCAL forms only on this one.
  if (cmd.startsWith("TIMER_LOCAL ")) {
    uint32_t seconds = cmd.substring(12).toInt();
    if (!seconds || seconds > 86400) return "ERR 1..86400 SEC";
    setTimerSeconds(seconds);
    return statusText();
  }
  if (cmd.startsWith("TIMER ")) {
    uint32_t seconds = cmd.substring(6).toInt();
    if (!seconds || seconds > 86400) return "ERR 1..86400 SEC";
    setTimerSeconds(seconds);
    scheduleControl(TIMER, seconds);
    return statusText();
  }
  if (cmd == "STOP_LOCAL") {
    stopAlarm();
    return "STOPPED HERE";
  }
  if (cmd == "STOP") {
    stopAlarm();
    scheduleControl(STOP, 0);
    return "STOPPED";
  }
  if (cmd.startsWith("VOLUME ")) {
    int amount = cmd.substring(7).toInt();
    if (amount < 1 || amount > 80) return "ERR VOLUME 1..80";
    volumePercent = amount;
    scheduleControl(VOLUME, amount);
    return "VOLUME " + String(amount);
  }
  if (cmd.startsWith("LED ")) {   // LED <day %> <night %>, this box only, saved
    int space = cmd.indexOf(' ', 4);
    int day = cmd.substring(4, space < 0 ? cmd.length() : space).toInt();
    int night = space < 0 ? ledNightPercent : cmd.substring(space + 1).toInt();
    if (day < 0 || day > 100 || night < 0 || night > 100) return "ERR LED 0..100 0..100";
    ledDayPercent = day;
    ledNightPercent = night;
    Preferences prefs;
    if (prefs.begin("house", false)) {
      prefs.putUChar("led_day", day);
      prefs.putUChar("led_night", night);
      prefs.end();
    }
    return "LED DAY " + String(day) + "% NIGHT " + String(night) + "%";
  }
  if (cmd == "STATUS") return statusText();
  return "ERR COMMAND";
}

#if HOUSE_ENABLE_BLE
class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *ch) override {
    std::string raw = ch->getValue();
    String input(raw.c_str());
    input.trim();
    String prefix = String(HOUSE_CONTROL_CODE) + " ";
    if (!input.startsWith(prefix)) {
      updateBleStatus("ERR CODE");
      return;
    }
    updateBleStatus(runCommand(input.substring(prefix.length())));
  }
};
#endif

bool peerFresh(const Peer &p, uint32_t nowMs) {
  return p.ip && nowMs - p.seenMs < PEER_TIMEOUT_MS;
}

// Unicast destinations for a packet; zero means "fall back to broadcast".
uint8_t collectTargets(uint8_t type, uint32_t value, uint32_t *out) {
  // HELLO (like controls) is broadcast and also unicast to known boxes: a lost
  // broadcast must not make a box forget its neighbours.
  uint32_t nowMs = millis();
  uint8_t own = roomNumber;
  uint8_t count = 0;
  portENTER_CRITICAL(&peerMux);
  if (type == VOICE && value >= 1 && value <= ASSISTANT_ROOM) {
    if (value != own && peerFresh(peers[value], nowMs)) out[count++] = peers[value].ip;
  } else if (type == ASSIST) {
    if (peerFresh(peers[ASSISTANT_ROOM], nowMs)) out[count++] = peers[ASSISTANT_ROOM].ip;
  } else {
    for (uint8_t room = 1; room <= 5; ++room)
      if (room != own && peerFresh(peers[room], nowMs)) out[count++] = peers[room].ip;
  }
  portEXIT_CRITICAL(&peerMux);
  return count;
}

// Target 1..6 must have sent HELLO recently; target 0 ("all") needs any box.
bool targetReachable(uint8_t target) {
  uint32_t unused[5];
  return collectTargets(VOICE, target, unused) > 0;
}

bool sendRaw(uint32_t ipNet, const uint8_t *data, size_t length) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(UDP_PORT);
  to.sin_addr.s_addr = ipNet;
  return sendto(txSock, data, length, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) ==
         int(length);
}

void sendPacket(uint8_t type, uint32_t value, const int16_t *samples = nullptr) {
  if (!udpReady || !udpTxMutex) {
    ++txFailed;
    return;
  }
  if (xSemaphoreTake(udpTxMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    ++txBusy;
    return;
  }
  if (txSock < 0) {
    xSemaphoreGive(udpTxMutex);
    ++txFailed;
    return;
  }
  uint32_t startedUs = micros();
  static uint8_t packet[sizeof(PacketHeader) + FRAME_BYTES]; // guarded by udpTxMutex
  // Only voice frames are numbered, so control packets no longer look like gaps.
  PacketHeader h{MAGIC, 2, type, uint16_t(type == VOICE ? ++sequenceNo : 0), nodeId, value};
  memcpy(packet, &h, sizeof(h));
  size_t length = sizeof(h);
  if (samples) {
    memcpy(packet + length, samples, FRAME_BYTES);
    length += FRAME_BYTES;
  }
  uint32_t targets[5];
  uint8_t count = collectTargets(type, value, targets);
  bool sent = true;
  // Controls also go by broadcast, so boxes not yet discovered still get them.
  if ((!count || type != VOICE) && type != ASSIST) sent = sendRaw(broadcastNet, packet, length);
  for (uint8_t i = 0; i < count; ++i) sent = sendRaw(targets[i], packet, length) && sent;
  if (sent) ++txSent;
  else ++txFailed;
  uint32_t elapsedUs = micros() - startedUs;
  if (elapsedUs > txMaxSendUs) txMaxSendUs = elapsedUs;
  if (elapsedUs > 20000) ++txSlow;
  xSemaphoreGive(udpTxMutex);
}

String peersText() {
  String text;
  uint32_t nowMs = millis();
  portENTER_CRITICAL(&peerMux);
  Peer copy[ASSISTANT_ROOM + 1];
  memcpy(copy, peers, sizeof(copy));
  portEXIT_CRITICAL(&peerMux);
  for (uint8_t room = 1; room <= ASSISTANT_ROOM; ++room) {
    if (!peerFresh(copy[room], nowMs)) continue;
    if (text.length()) text += ' ';
    text += String(roomShortName(room)) + "=" + IPAddress(copy[room].ip).toString();
  }
  return text.length() ? text : String("none");
}

void handlePacket(const uint8_t *buffer, int got, uint32_t fromIp) {
  ++rxPackets;
  if (got < int(sizeof(PacketHeader))) return;
  PacketHeader h;
  memcpy(&h, buffer, sizeof(h));
  if (h.magic != MAGIC || h.version != 2 || h.origin == nodeId) return;
  ++rxValid;
  if (h.type == HELLO && got == int(sizeof(h)) && h.value >= 1 &&
      h.value <= ASSISTANT_ROOM) {
    portENTER_CRITICAL(&peerMux);
    peers[h.value] = {fromIp, h.origin, millis()};
    portEXIT_CRITICAL(&peerMux);
    return;
  }
  if (h.type == TIMER && got == int(sizeof(h)) && h.value >= 1 && h.value <= 86400) {
    ++rxControls;
    setTimerSeconds(h.value);
  } else if (h.type == STOP && got == int(sizeof(h))) {
    ++rxControls;
    stopAlarm();
  } else if (h.type == VOLUME && got == int(sizeof(h)) && h.value >= 1 && h.value <= 80) {
    ++rxControls;
    volumePercent = h.value;
  } else if (h.type == VOICE && got == int(sizeof(h) + FRAME_BYTES)) {
    const int16_t *samples = reinterpret_cast<const int16_t *>(buffer + sizeof(h));
    if (h.value == 0 || h.value == roomNumber) {
      if (alarmActive || localTalking) {
        ++rxVoiceBlocked;
        return;
      }
      uint32_t now = millis();
      if (assistMuted && h.origin == peers[ASSISTANT_ROOM].node) {
        ++rxVoiceBlocked;   // the user cancelled this answer
        return;
      }
      bool newStream = h.origin != lastVoiceOrigin ||
                       now - lastVoiceArrivalMs > 400;
      if (newStream) {
        clearQueue();
        // Frames keep queuing while the cue plays, so no speech is lost.
        bool fromAssistant = h.origin == peers[ASSISTANT_ROOM].node;
        if (now - lastVoiceArrivalMs > CUE_QUIET_MS && !fromAssistant) toneRequest = TONE_CUE;
      } else {
        uint16_t delta = uint16_t(h.sequence - lastVoiceSequence);
        if (delta == 0 || delta > 100) {
          ++rxVoiceReordered;
          return; // do not play duplicate or late audio after newer frames
        }
        if (delta > 1) rxVoiceSeqGaps += delta - 1;
        uint32_t gapMs = now - lastVoiceArrivalMs;
        if (gapMs > rxMaxInterpacketMs) rxMaxInterpacketMs = gapMs;
      }
      lastVoiceOrigin = h.origin;
      lastVoiceSequence = h.sequence;
      lastVoiceArrivalMs = now;
      remoteVoiceLastMs = now;
      if (h.origin == peers[ASSISTANT_ROOM].node) lastAssistantVoiceMs = now;
      ++rxVoiceAccepted;
      pushFrame(samples);
    } else {
      ++rxVoiceOtherRoom;
    }
  }
}

// Stop the assistant's answer on this box and tell the assistant to drop it.
void cancelAssistant() {
  assistMuted = true;
  if (millis() - lastAssistantVoiceMs < 1000) clearQueue();
  sendPacket(ASSIST, ASSIST_CANCEL);
  sendPacket(ASSIST, ASSIST_CANCEL);
}

bool initI2s() {
  i2s_config_t out = {};
  out.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  out.sample_rate = SAMPLE_RATE;
  out.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  out.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  out.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  out.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  out.dma_buf_count = 8;
  out.dma_buf_len = FRAME_SAMPLES;
  out.use_apll = false;
  out.tx_desc_auto_clear = true;
  i2s_pin_config_t outPins = {};
  outPins.bck_io_num = AMP_BCLK;
  outPins.ws_io_num = AMP_WS;
  outPins.data_out_num = ampDataPin;
  outPins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_driver_install(I2S_NUM_0, &out, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &outPins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);

  i2s_config_t in = {};
  in.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  in.sample_rate = SAMPLE_RATE;
  in.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // The ICS-43434 needs a full 64-clock stereo frame even with one microphone.
  // Inspect both slots until the physical SEL connection has been confirmed.
  in.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  in.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  in.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  in.dma_buf_count = 4;
  in.dma_buf_len = FRAME_SAMPLES;
  in.use_apll = false;
  i2s_pin_config_t inPins = {};
  inPins.bck_io_num = MIC_BCLK;
  inPins.ws_io_num = MIC_WS;
  inPins.data_out_num = I2S_PIN_NO_CHANGE;
  inPins.data_in_num = micDataPin;
  if (i2s_driver_install(I2S_NUM_1, &in, 0, nullptr) != ESP_OK) return false;
  return i2s_set_pin(I2S_NUM_1, &inPins) == ESP_OK;
}

void playbackTask(void *) {
  static int16_t mono[FRAME_SAMPLES];
  static int16_t stereo[FRAME_SAMPLES * 2];
  File alarmFile;
  bool wasRinging = false;
  uint32_t fallbackPhase = 0;
  uint8_t tone = TONE_NONE;
  uint32_t toneFrame = 0;
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    bool ringing = alarmActive;
    if (ringing && !wasRinging) {
      clearQueue();
      alarmFile = LittleFS.open("/alarm.raw", "r");
      fallbackPhase = 0;
    } else if (!ringing && wasRinging) {
      if (alarmFile) alarmFile.close();
      clearQueue();
    }
    wasRinging = ringing;
    if (ringing) {
      if (alarmFile) {
        size_t count = alarmFile.read(reinterpret_cast<uint8_t *>(mono), FRAME_BYTES);
        if (count < FRAME_BYTES) {
          alarmFile.seek(0);
          count += alarmFile.read(reinterpret_cast<uint8_t *>(mono) + count, FRAME_BYTES - count);
        }
        if (count < FRAME_BYTES) memset(reinterpret_cast<uint8_t *>(mono) + count, 0, FRAME_BYTES - count);
      } else {
        // Safe fallback when the custom melody has not yet been flashed.
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
          uint32_t t = fallbackPhase++ % 16000;
          mono[i] = t < 12000 ? int16_t(8500 * sinf(2 * PI * 660 * t / SAMPLE_RATE)) : 0;
        }
      }
    } else {
      if (!tone && toneRequest) {
        tone = toneRequest;
        toneRequest = TONE_NONE;
        toneFrame = 0;
      }
      if (!tone || !fillToneFrame(tone, toneFrame++, mono)) {
        tone = TONE_NONE;
        if (localTalking || !popFrame(mono)) memset(mono, 0, sizeof(mono));
      }
    }
    int vol = volumePercent;
    // Night mode quietens beeps and the alarm only; speech keeps its volume.
    if (nightMode && (ringing || tone)) vol = vol * HOUSE_NIGHT_VOLUME_SHARE / 100;
    for (int i = 0; i < FRAME_SAMPLES; ++i) {
      int16_t sample = clip16(int32_t(mono[i]) * vol / 100);
      stereo[2 * i] = sample;
      stereo[2 * i + 1] = sample;
    }
    size_t written = 0;
    i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written, portMAX_DELAY);
  }
}

void captureTask(void *) {
  static int32_t micRaw[FRAME_SAMPLES * 2];
  static int16_t out[FRAME_SAMPLES];
  uint8_t candidateButton = 255;
  uint8_t stableButton = 255;
  uint8_t lastTapButton = 255;
  uint8_t tapCount = 0;
  bool swallowPress = false;
  bool assistantRequest = false;
  bool assistOn = false, assistHandsFree = false, assistHeard = false;
  uint32_t assistStartMs = 0, assistLastSpeechMs = 0, assistFloor = 0, assistReportMs = 0;
  uint8_t assistPressTicks = 0;
  bool assistHeldBefore = false;
  bool assistConversation = false, assistAnswerSeen = false;
  uint32_t assistWaitSinceMs = 0;
  uint8_t queuedChimeTarget = 255;
  uint8_t chimeTarget = 255;
  uint32_t candidateSince = 0;
  uint32_t pressedSince = 0;
  uint32_t lastTapReleased = 0;
  uint32_t chimeFrame = CHIME_FRAMES;
  uint64_t micAbsSum[2] = {0, 0};
  uint32_t micSamples = 0;
  uint32_t micPeak[2] = {0, 0};
  uint32_t micRawOr[2] = {0, 0};
  uint32_t lastMicReportMs = 0;
  uint32_t lastCaptureMs = 0;
  TickType_t next = xTaskGetTickCount();
  esp_task_wdt_add(nullptr);
  for (;;) {
    vTaskDelayUntil(&next, pdMS_TO_TICKS(20));
    esp_task_wdt_reset();
    uint32_t nowMs = millis();
    if (lastCaptureMs) {
      uint32_t periodMs = nowMs - lastCaptureMs;
      if (periodMs > captureMaxPeriodMs) captureMaxPeriodMs = periodMs;
      if (periodMs > 30) ++captureLateTicks;
    }
    lastCaptureMs = nowMs;
    // Debounce and identify one button at a time. Short double taps send a
    // melody; holding longer than SHORT_TAP_MS keeps push-to-talk available.
    uint8_t rawButton = 255;
    if (digitalRead(allButtonPin) == LOW) {
      rawButton = 0;
    } else {
      for (uint8_t i = 0; i < 4; ++i) {
        if (digitalRead(PRIVATE_BUTTON_PINS[i]) == LOW) {
          rawButton = i + 1;
          break;
        }
      }
    }
    if (rawButton != candidateButton) {
      candidateButton = rawButton;
      candidateSince = nowMs;
    }
    if (candidateButton != stableButton && nowMs - candidateSince >= 40) {
      uint8_t previous = stableButton;
      stableButton = candidateButton;
      if (previous != 255) {
        if (nowMs - pressedSince <= SHORT_TAP_MS) {
          if (lastTapButton == previous && nowMs - lastTapReleased <= DOUBLE_TAP_GAP_MS) {
            ++tapCount;
          } else {
            lastTapButton = previous;
            tapCount = 1;
          }
          lastTapReleased = nowMs;
        } else {
          lastTapButton = 255;  // a long press (talk) ends any tap sequence
          tapCount = 0;
        }
      }
      if (stableButton != 255) pressedSince = nowMs;
      // While the alarm rings, any button just silences it on this box; the
      // press is swallowed so it neither talks nor counts as a tap.
      if (stableButton != 255 && alarmActive) {
        stopAlarm();
        swallowPress = true;
        Serial.println("ALARM stopped by button");
      }
    }
    if (swallowPress) {
      if (stableButton == 255) swallowPress = false;  // released
      lastTapButton = 255;
      tapCount = 0;
      continue;
    }
    // A tap sequence is decided once no further tap follows: two taps send the
    // melody, three taps of the big button ask the assistant.
    if (lastTapButton != 255 && stableButton == 255 &&
        nowMs - lastTapReleased > DOUBLE_TAP_GAP_MS) {
      if (tapCount == 2)
        queuedChimeTarget = lastTapButton == 0 ? 0 : privateTargetForButton(lastTapButton - 1);
      else if (tapCount >= 3 && lastTapButton == 0)
        assistantRequest = true;
      lastTapButton = 255;
      tapCount = 0;
    }

    bool online = udpReady && WiFi.status() == WL_CONNECTED;
    bool busy = nowMs - remoteVoiceLastMs < 350;
    if (usbChimeTarget != 255) {
      queuedChimeTarget = usbChimeTarget;
      usbChimeTarget = 255;
    }
    if (queuedChimeTarget != 255) {
      if (!online || !targetReachable(queuedChimeTarget)) {
        toneRequest = TONE_UNAVAILABLE;
        deniedTarget = queuedChimeTarget;
        deniedAtMs = nowMs;
        Serial.printf("CHIME SKIPPED: room %u offline\n", unsigned(queuedChimeTarget));
      } else if (!busy && !alarmActive) {
        chimeTarget = queuedChimeTarget;
        chimeFrame = 0;
        Serial.printf("CHIME SEND target=%u\n", unsigned(chimeTarget));
      } else {
        Serial.println("CHIME SKIPPED: device busy");
      }
      queuedChimeTarget = 255;
    }
    if (chimeFrame < CHIME_FRAMES) {
      if (online) {
        localTalking = true;
        activeTarget = chimeTarget;
        fillTestChimeFrame(out, chimeFrame++);
        // The receiver cannot tell the melody from speech, so the sender
        // quietens it at night (all boxes share the same NTP clock).
        if (nightMode)
          for (int i = 0; i < FRAME_SAMPLES; ++i)
            out[i] = int16_t(int32_t(out[i]) * HOUSE_NIGHT_VOLUME_SHARE / 100);
        sendPacket(VOICE, chimeTarget, out);
        continue;
      }
      chimeFrame = CHIME_FRAMES;
    }

    // Dedicated assistant button (optional): hold to ask, release to send.
    bool assistHeld = false;
    if (assistPin >= 0) {
      assistPressTicks = digitalRead(assistPin) == LOW ?
          uint8_t(assistPressTicks < 3 ? assistPressTicks + 1 : 3) : 0;
      assistHeld = assistPressTicks >= 3;  // 60 ms debounce
    }
    if (usbAssistRequest) {
      usbAssistRequest = false;
      assistantRequest = true;
    }
    bool assistPressEdge = assistHeld && !assistHeldBefore;
    assistHeldBefore = assistHeld;
    // Conversation: once the answer has finished playing, listen for the next
    // question without a tap. Silence or any button ends the conversation.
    if (assistConversation && !assistOn) {
      if (stableButton != 255) {
        assistConversation = false;
        cancelAssistant();
        swallowPress = true;   // the press only ends the conversation
        Serial.println("ASSIST conversation ended by button");
        continue;
      } else {
        uint32_t lastAnswer = lastAssistantVoiceMs;
        if (int32_t(lastAnswer - assistWaitSinceMs) > 0) assistAnswerSeen = true;
        if (assistAnswerSeen && nowMs - lastAnswer > 700 && playbackIdle()) {
          assistantRequest = true;
        } else if (!assistAnswerSeen && nowMs - assistWaitSinceMs > 20000) {
          assistConversation = false;
          toneRequest = TONE_UNAVAILABLE;
          Serial.println("ASSIST no answer within 20 s");
        }
      }
    }
    if ((assistantRequest || assistPressEdge) && !assistOn) {
      if (!online || !targetReachable(ASSISTANT_ROOM)) {
        toneRequest = TONE_UNAVAILABLE;
        deniedTarget = ASSISTANT_ROOM;
        deniedAtMs = nowMs;
        Serial.println("ASSIST SKIPPED: assistant offline");
      } else if (!alarmActive) {
        assistOn = true;
        assistHandsFree = !assistPressEdge;
        assistHeard = false;
        assistStartMs = assistLastSpeechMs = nowMs;
        assistFloor = 0;
        assistMuted = false;
        toneRequest = TONE_CUE;  // "speak now"
        Serial.printf("ASSIST START %s\n", assistHandsFree ? "hands-free" : "button");
      }
    }
    assistantRequest = false;
    if (assistOn) {
      uint32_t elapsed = nowMs - assistStartMs;
      bool cancel = stableButton != 255;  // any intercom button cancels
      bool finished = assistHandsFree ?
          (assistHeard && nowMs - assistLastSpeechMs > ASSIST_SILENCE_MS) ||
              (!assistHeard && elapsed > ASSIST_NO_SPEECH_MS) || elapsed > ASSIST_MAX_MS :
          !assistHeld || elapsed > MAX_TALK_MS;
      if (cancel || finished) {
        assistOn = false;
        localTalking = false;
        activeTarget = 255;
        // Tell the user the question left the box, so the wait for the answer
        // does not look like a hang; low beeps when nothing was heard.
        if (cancel) {
          assistConversation = false;
          cancelAssistant();
          swallowPress = true;   // the press only cancels, it does not talk
        } else if (assistHandsFree && !assistHeard) {
          assistConversation = false;   // silence: the conversation is over
          toneRequest = TONE_UNAVAILABLE;
        } else {
          assistConversation = true;    // wait for the answer, then listen again
          assistAnswerSeen = false;
          assistWaitSinceMs = nowMs;
          toneRequest = TONE_SENT;
          // Tell the assistant at once instead of letting it wait for the stream
          // to go quiet; twice, as a lost packet only costs that wait.
          sendPacket(ASSIST, ASSIST_DONE);
          sendPacket(ASSIST, ASSIST_DONE);
        }
        Serial.printf("ASSIST END after %lu ms, speech=%u%s\n", (unsigned long)elapsed,
                      unsigned(assistHeard), cancel ? " (cancelled)" : "");
        continue;
      }
      localTalking = true;
      activeTarget = ASSISTANT_ROOM;
      size_t got = 0;
      i2s_read(I2S_NUM_1, micRaw, sizeof(micRaw), &got, pdMS_TO_TICKS(25));
      if (got != sizeof(micRaw)) {
        ++micReadShort;
        continue;
      }
      ++micReadFull;
      uint64_t laneSum[2] = {0, 0};
      for (int i = 0; i < FRAME_SAMPLES * 2; ++i) {
        int32_t s = micRaw[i] >> 16;
        laneSum[i & 1] += uint32_t(s < 0 ? -s : s);
      }
      int lane = laneSum[1] > laneSum[0] ? 1 : 0;
      uint64_t level = 0;
      for (int i = 0; i < FRAME_SAMPLES; ++i) {
        out[i] = clip16((micRaw[2 * i + lane] >> 16) * 6);
        level += uint32_t(out[i] < 0 ? -out[i] : out[i]);
      }
      uint32_t avg = uint32_t(level / FRAME_SAMPLES);
      if (elapsed < ASSIST_SKIP_MS) continue;  // our own start beep
      // Simple voice activity detection: the quietest recent level is the room
      // noise; speech is clearly above it.
      if (!assistFloor || avg < assistFloor) assistFloor = avg;
      else assistFloor += (avg - assistFloor) / 200 + 1;
      uint32_t threshold = assistFloor * 3 > 300 ? assistFloor * 3 : 300;
      if (avg > threshold) {
        assistHeard = true;
        assistLastSpeechMs = nowMs;
      }
      if (nowMs - assistReportMs >= 250) {
        assistReportMs = nowMs;
        Serial.printf("ASSIST level=%lu floor=%lu speech=%u\n", (unsigned long)avg,
                      (unsigned long)assistFloor, unsigned(assistHeard));
      }
      sendPacket(VOICE, ASSISTANT_ROOM, out);
      continue;
    }

    uint8_t target = stableButton == 0 ? 0 :
                     stableButton <= 4 ? privateTargetForButton(stableButton - 1) : 255;
    // A stuck button or shorted wire must not keep this box transmitting
    // forever: while it talks it also refuses all incoming voice.
    bool stuck = target != 255 && nowMs - pressedSince > MAX_TALK_MS;
    static bool stuckReported = false;
    if (stuck && !stuckReported) Serial.printf("BUTTON STUCK: K%u held over 60 s, talk stopped\n",
                                               unsigned(stableButton));
    stuckReported = stuck;
    bool wantsTalk = target != 255 && nowMs - pressedSince > SHORT_TAP_MS && !stuck &&
                     !busy && !alarmActive;
    // Talking into an offline box is pointless: beep low once per press and
    // stay silent. If the box reappears while the button is held, talk starts.
    static uint32_t deniedPressSince = UINT32_MAX;
    if (wantsTalk && (!online || !targetReachable(target))) {
      if (deniedPressSince != pressedSince) {
        deniedPressSince = pressedSince;
        toneRequest = TONE_UNAVAILABLE;
        deniedTarget = target;
        deniedAtMs = nowMs;
        Serial.printf("TALK SKIPPED: room %u offline\n", unsigned(target));
      }
      wantsTalk = false;
    }
    localTalking = wantsTalk;
    activeTarget = localTalking ? target : 255;
    bool micTesting = micTestUntilMs && int32_t(micTestUntilMs - nowMs) > 0;
    if (!micTesting) {
      micAbsSum[0] = micAbsSum[1] = 0;
      micSamples = 0;
      micPeak[0] = micPeak[1] = 0;
      micRawOr[0] = micRawOr[1] = 0;
    }
    if (localTalking || micTesting) {
      size_t got = 0;
      i2s_read(I2S_NUM_1, micRaw, sizeof(micRaw), &got, pdMS_TO_TICKS(25));
      if (got == sizeof(micRaw)) {
        ++micReadFull;
        uint64_t frameSum[2] = {0, 0};
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
          for (int lane = 0; lane < 2; ++lane) {
            int32_t raw = micRaw[2 * i + lane];
            int32_t sample = raw >> 16;
            uint32_t magnitude = sample < 0 ? uint32_t(-sample) : uint32_t(sample);
            frameSum[lane] += magnitude;
            if (micTesting) {
              micAbsSum[lane] += magnitude;
              micRawOr[lane] |= uint32_t(raw);
              if (magnitude > micPeak[lane]) micPeak[lane] = magnitude;
            }
          }
        }
        if (micTesting) micSamples += FRAME_SAMPLES;
        if (localTalking) {
          int lane = frameSum[1] > frameSum[0] ? 1 : 0;
          for (int i = 0; i < FRAME_SAMPLES; ++i)
            out[i] = clip16((micRaw[2 * i + lane] >> 16) * 6);
          sendPacket(VOICE, target, out);
        }
      } else {
        ++micReadShort;
      }
      if (micTesting && millis() - lastMicReportMs >= 250) {
        if (micSamples) {
          Serial.printf("MIC L0 avg=%lu peak=%lu raw=%08lX | L1 avg=%lu peak=%lu raw=%08lX samples=%lu\n",
                        (unsigned long)(micAbsSum[0] / micSamples),
                        (unsigned long)micPeak[0], (unsigned long)micRawOr[0],
                        (unsigned long)(micAbsSum[1] / micSamples),
                        (unsigned long)micPeak[1], (unsigned long)micRawOr[1],
                        (unsigned long)micSamples);
        } else {
          Serial.println("MIC NO DATA");
        }
        micAbsSum[0] = micAbsSum[1] = 0;
        micSamples = 0;
        micPeak[0] = micPeak[1] = 0;
        micRawOr[0] = micRawOr[1] = 0;
        lastMicReportMs = millis();
      }
    }
  }
}

#if HOUSE_ENABLE_BLE
void initBle() {
  NimBLEDevice::init(deviceName);
  NimBLEServer *server = NimBLEDevice::createServer();
  NimBLEService *service = server->createService(SERVICE_UUID);
  NimBLECharacteristic *command = service->createCharacteristic(
      COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  command->setCallbacks(new CommandCallbacks());
  statusCharacteristic = service->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusCharacteristic->setValue("IDLE");
  service->start();
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
}
#endif

void closeSockets() {
  if (rxSock >= 0) close(rxSock);
  rxSock = -1;
  if (xSemaphoreTake(udpTxMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (txSock >= 0) close(txSock);
    txSock = -1;
    xSemaphoreGive(udpTxMutex);
  }
  udpReady = false;
}

bool openSockets(IPAddress ip) {
  int one = 1;
  int rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (rx < 0 || tx < 0) {
    if (rx >= 0) close(rx);
    if (tx >= 0) close(tx);
    return false;
  }
  // ESP-IDF lwIP delivers broadcasts only to sockets with SO_BROADCAST set.
  setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(rx, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  setsockopt(tx, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  timeval timeout = {0, 100 * 1000};
  setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_port = htons(UDP_PORT);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(rx, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
    close(rx);
    close(tx);
    return false;
  }
  broadcastNet = uint32_t(ip) | ~uint32_t(WiFi.subnetMask());
  rxSock = rx;
  if (xSemaphoreTake(udpTxMutex, portMAX_DELAY) == pdTRUE) {
    txSock = tx;
    xSemaphoreGive(udpTxMutex);
  }
  boundIp = ip;
  udpReady = true;
  return true;
}

// Receives on its own task, so every UDP frame is drained the moment it
// arrives; lwIP keeps only about six datagrams per socket. Polling one packet
// per loop() pass dropped frames whenever the router delivered a burst.
void netTask(void *) {
  static uint8_t buffer[sizeof(PacketHeader) + FRAME_BYTES + 4];
  uint32_t lastHelloMs = 0;
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    bool connected = WiFi.status() == WL_CONNECTED;
    IPAddress ip = connected ? WiFi.localIP() : IPAddress();
    if (udpReady && (!connected || ip != boundIp)) {
      closeSockets();
      Serial.println("Wi-Fi lost or IP changed: UDP sockets closed");
    }
    if (!udpReady) {
      if (connected && uint32_t(ip) && openSockets(ip)) {
        Serial.printf("UDP ready on %s, broadcast %s\n", ip.toString().c_str(),
                      IPAddress(broadcastNet).toString().c_str());
        lastHelloMs = millis() - HELLO_INTERVAL_MS;
      } else {
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
    }
    if (roomNumber && millis() - lastHelloMs >= HELLO_INTERVAL_MS) {
      lastHelloMs = millis();
      sendPacket(HELLO, roomNumber);
    }
    sockaddr_in from = {};
    socklen_t fromLength = sizeof(from);
    int got = recvfrom(rxSock, buffer, sizeof(buffer), 0,
                       reinterpret_cast<sockaddr *>(&from), &fromLength);
    if (got > 0) handlePacket(buffer, got, from.sin_addr.s_addr);
  }
}

const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Переговорка</title>
<style>
body{font-family:system-ui,sans-serif;max-width:420px;margin:0 auto;padding:16px;background:#f4f4f2;color:#222}
h1{font-size:20px}label{display:block;margin:14px 0 4px}
input,button{font-size:18px;padding:10px;width:100%;box-sizing:border-box;border-radius:8px;border:1px solid #bbb}
button{background:#2d6cdf;color:#fff;border:0;margin-top:8px}
button.stop{background:#c62828}#out{margin-top:16px;padding:10px;background:#fff;border-radius:8px;min-height:1.4em}
</style></head><body>
<h1 id="name">Переговорка</h1>
<label>Код управления</label><input id="code" type="password" inputmode="numeric">
<label>Таймер, минут</label><input id="min" type="number" min="1" max="1440" value="10">
<label style="display:flex;gap:8px;align-items:center"><input id="here" type="checkbox" checked style="width:auto">только эта коробка</label>
<button onclick="send((document.getElementById('here').checked?'TIMER_LOCAL ':'TIMER ')+Math.round(document.getElementById('min').value*60))">Запустить таймер</button>
<button class="stop" onclick="send('STOP')">СТОП (везде)</button>
<label>Громкость везде, 1–80</label><input id="vol" type="number" min="1" max="80" value="80">
<button onclick="send('VOLUME '+document.getElementById('vol').value)">Применить громкость</button>
<button onclick="send('STATUS')">Состояние</button>
<div id="out"></div>
<p><a href="/status">Все коробки: кто в сети, сигнал Wi‑Fi →</a></p>
<script>
var c=document.getElementById('code');
try{c.value=localStorage.getItem('code')||''}catch(e){}
fetch('/name').then(function(r){return r.text()}).then(function(t){document.getElementById('name').textContent=t});
function send(cmd){
  try{localStorage.setItem('code',c.value)}catch(e){}
  fetch('/cmd?code='+encodeURIComponent(c.value)+'&c='+encodeURIComponent(cmd))
    .then(function(r){return r.text()})
    .then(function(t){document.getElementById('out').textContent=t})
    .catch(function(){document.getElementById('out').textContent='Нет связи с коробкой'});
}
</script></body></html>)HTML";

// Every box serves /info with CORS, so the status page on any box can ask
// all the others directly from the phone's browser.
const char STATUS_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Коробки</title>
<style>
body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#f4f4f2;color:#222}
h1{font-size:20px}.box{background:#fff;border-radius:10px;padding:10px 12px;margin:8px 0;border-left:6px solid #bbb}
.ok{border-color:#2e7d32}.warn{border-color:#f9a825}.bad{border-color:#c62828}
.n{font-weight:700;font-size:18px}.d{color:#555;font-size:14px;margin-top:4px}
</style></head><body>
<h1>Переговорка: все коробки</h1>
<div id="list">Загрузка…</div>
<p class="d">Обновляется каждые 5 секунд. Сигнал: лучше −65 — отлично, −65…−72 — нормально, хуже −75 — возможны потери.</p>
<p><a href="/">← Управление</a></p>
<script>
var L={1:'1F — первый этаж',2:'2F — второй этаж',3:'3F-A — третий, комната 1',4:'3F-B — третий, комната 2',5:'4F — четвёртый этаж'};
function get(u){var c=new AbortController();setTimeout(function(){c.abort()},2500);
  return fetch(u,{signal:c.signal}).then(function(r){if(!r.ok)throw 0;return r.json()})}
function up(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h+' ч '+m+' мин'}
function card(room,cls,text){return '<div class="box '+cls+'"><div class="n">'+L[room]+'</div><div class="d">'+text+'</div></div>'}
function show(info){
  var rows={};rows[info.room]=info;var known={};known[info.room]=info.ip;
  info.peers.forEach(function(p){known[p.room]=p.ip});
  var jobs=info.peers.map(function(p){return get('http://'+p.ip+'/info').then(function(i){rows[p.room]=i},function(){rows[p.room]={old:true,ip:p.ip}})});
  Promise.all(jobs).then(function(){
    var html='';
    for(var r=1;r<=5;r++){
      var i=rows[r];
      if(!i){html+=card(r,'bad','Нет в сети');continue}
      if(i.old){html+=card(r,'warn','В сети ('+i.ip+'), старая прошивка без страницы состояния');continue}
      var cls=i.rssi>-72?'ok':'warn';
      html+=card(r,cls,i.ip+' · сигнал '+i.rssi+' дБм · работает '+up(i.uptime)+'<br>последний запуск: '+i.reset+' · прошивка '+i.fw+
        '<br>время '+(i.time=='unknown'?'неизвестно':i.time)+(i.night?' · ночной режим, тише в 2 раза':'')+(r==info.room?' · (эта коробка)':''));
    }
    document.getElementById('list').innerHTML=html;
  });
}
function tick(){get('/info').then(show,function(){document.getElementById('list').textContent='Нет связи с коробкой'})}
tick();setInterval(tick,5000);
</script></body></html>)HTML";

// False until NTP has delivered a real date: no clock means normal volume.
bool localTimeKnown(tm &local) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false; // before 2023: clock not set yet
  localtime_r(&now, &local);
  return true;
}

bool isNightNow() {
  tm local;
  if (!localTimeKnown(local)) return false;
  int minutes = local.tm_hour * 60 + local.tm_min;
  return minutes >= HOUSE_NIGHT_START_MIN || minutes < HOUSE_NIGHT_END_MIN;
}

String clockText() {
  tm local;
  if (!localTimeKnown(local)) return "unknown";
  char text[8];
  snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
  return text;
}

const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "включение питания";
    case ESP_RST_SW: return "перезапуск программой";
    case ESP_RST_PANIC: return "сбой программы";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "сторожевой таймер (зависание)";
    case ESP_RST_BROWNOUT: return "просадка питания";
    case ESP_RST_EXT: return "кнопка EN";
    default: return "другое";
  }
}

String infoJson() {
  char head[400];
  snprintf(head, sizeof(head),
           "{\"name\":\"%s\",\"room\":%u,\"ip\":\"%s\",\"fw\":\"%s\",\"rssi\":%d,"
           "\"uptime\":%lu,\"reset\":\"%s\",\"heap\":%lu,\"min_heap\":%lu,"
           "\"voice_rx\":%lu,\"seq_gaps\":%lu,\"time\":\"%s\",\"night\":%s,\"peers\":[",
           deviceName, unsigned(roomNumber), WiFi.localIP().toString().c_str(), FW_VERSION,
           WiFi.RSSI(), (unsigned long)(millis() / 1000), resetReasonText(),
           (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)rxVoiceAccepted, (unsigned long)rxVoiceSeqGaps,
           clockText().c_str(), nightMode ? "true" : "false");
  String json(head);
  uint32_t nowMs = millis();
  portENTER_CRITICAL(&peerMux);
  Peer copy[ASSISTANT_ROOM + 1];
  memcpy(copy, peers, sizeof(copy));
  portEXIT_CRITICAL(&peerMux);
  bool first = true;
  for (uint8_t room = 1; room <= 5; ++room) {
    if (room == roomNumber || !peerFresh(copy[room], nowMs)) continue;
    json += first ? "" : ",";
    first = false;
    json += "{\"room\":" + String(room) + ",\"ip\":\"" + IPAddress(copy[room].ip).toString() +
            "\",\"age\":" + String((nowMs - copy[room].seenMs) / 1000) + "}";
  }
  return json + "]}";
}

void startWeb() {
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  web.on("/status", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", STATUS_PAGE); });
  web.on("/info", HTTP_GET, [] {
    web.sendHeader("Access-Control-Allow-Origin", "*");
    web.send(200, "application/json; charset=utf-8", infoJson());
  });
  // Firmware over Wi-Fi: curl -F "firmware=@firmware.bin" "http://<ip>/update?code=CHANGE_ME"
  web.on("/update", HTTP_POST, [] {
    bool ok = otaAuthorized && otaWritten && !Update.hasError();
    web.sendHeader("Connection", "close");
    web.send(ok ? 200 : otaAuthorized ? 500 : 403, "text/plain",
             ok ? "UPDATE OK, REBOOTING\n" : otaAuthorized ? "UPDATE FAILED\n" : "ERR CODE\n");
    if (ok) {
      delay(500);
      ESP.restart();
    }
  }, [] {
    HTTPUpload &upload = web.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaAuthorized = web.arg("code") == HOUSE_CONTROL_CODE;
      otaWritten = false;
      if (otaAuthorized) {
        Serial.printf("OTA start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      esp_task_wdt_reset();
      if (otaAuthorized && Update.isRunning() &&
          Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (otaAuthorized && Update.isRunning()) {
        otaWritten = Update.end(true);
        if (otaWritten) Serial.printf("OTA done: %u bytes\n", unsigned(upload.totalSize));
        else Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      if (Update.isRunning()) Update.abort();
      Serial.println("OTA aborted");
    }
  });
  web.on("/name", HTTP_GET, [] { web.send(200, "text/plain", deviceName); });
  web.on("/stats", HTTP_GET, [] { web.send(200, "text/plain", statsText()); });
  web.on("/cmd", HTTP_GET, [] {
    if (web.arg("code") != HOUSE_CONTROL_CODE) {
      web.send(403, "text/plain", "ERR CODE");
      return;
    }
    web.send(200, "text/plain", runCommand(web.arg("c")));
  });
  web.begin();
  configTzTime(HOUSE_TIMEZONE, "pool.ntp.org", "time.cloudflare.com");
  String host = String("house-") + roomShortName(roomNumber);
  host.toLowerCase();
  if (MDNS.begin(host.c_str())) MDNS.addService("http", "tcp", 80);
  webStarted = true;
  Serial.printf("Web control: http://%s/ or http://%s.local/\n",
                WiFi.localIP().toString().c_str(), host.c_str());
}

// The eye sees brightness roughly as the square of the PWM duty, so 10 % looks
// like 10 % instead of nearly full.
void statusLed(bool on) {
  uint8_t percent = nightMode ? ledNightPercent : ledDayPercent;
  uint32_t duty = on ? uint32_t(1023.0f * percent * percent / 10000.0f + 0.5f) : 0;
  if (on && percent && !duty) duty = 1;
  ledcWrite(LED_CHANNEL, duty);
}

void updateStatusLed() {
  // Solid: ready (dark at night). Lit while a call or the alarm comes in.
  // Screenless rooms blink quickly while sending voice. Slow blink: no network.
  const bool online = udpReady && WiFi.status() == WL_CONNECTED;
  const bool incoming = millis() - remoteVoiceLastMs < 400 || alarmActive;
  if (roomNumber != 1 && online && localTalking) {
    statusLed((millis() / 150) % 2 == 0);
  } else if (!online) {
    statusLed((millis() / 500) % 2 == 0);
  } else {
    statusLed(!nightMode || incoming);
  }
}

[[noreturn]] void fatalBlink() {
  // A fast blink distinguishes a startup failure from a missing network.
  for (;;) {
    statusLed(true);
    delay(150);
    statusLed(false);
    delay(150);
  }
}

void connectWifi() {
  if (wifiPassword.isEmpty()) {
    Serial.println("Set Wi-Fi password locally over USB: python tools/set_wifi_password.py COM6");
    return;
  }
  WiFi.mode(WIFI_STA);
#if HOUSE_ENABLE_BLE
  // BLE coexistence requires modem sleep; disabling it aborted in
  // coex_core_enable during BLE init. Voice may stutter in this build.
#else
  // Stay awake: in modem sleep the router buffers frames until its DTIM
  // beacon (100-300 ms) and the 120 ms playback buffer runs dry.
  WiFi.setSleep(false);
#endif
  WiFi.setAutoReconnect(true);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(HOUSE_WIFI_SSID, wifiPassword.c_str());
    Serial.print("Wi-Fi connecting");
    for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
      delay(250);
      updateStatusLed();
      Serial.print('.');
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" Wi-Fi unavailable; retrying in 5 s");
    return;
  }
#if !HOUSE_ENABLE_BLE
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  Serial.printf("\nWi-Fi %s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}
} // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  // Any watched task (loop, speaker, microphone, net) silent for 30 s => reboot.
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);
  Serial.printf("Firmware %s, last reset: %s\n", FW_VERSION, resetReasonText());
  ledcSetup(LED_CHANNEL, 5000, 10);   // 5 kHz: no visible flicker
  ledcAttachPin(STATUS_LED_PIN, LED_CHANNEL);
  ledcWrite(LED_CHANNEL, 0);
  for (int pin : PRIVATE_BUTTON_PINS) pinMode(pin, INPUT_PULLUP);
  nodeId = uint32_t(ESP.getEfuseMac());
  Preferences prefs;
  if (prefs.begin("house", true)) {
    uint8_t stored = prefs.getUChar("room", 0);
    if (stored >= 1 && stored <= 5) roomNumber = stored;
    wifiPassword = prefs.getString("wifi_pwd", "");
    int storedAmp = prefs.getUChar("amp_din", DEFAULT_AMP_DATA);
    for (int pin : AMP_DATA_CHOICES)
      if (pin == storedAmp) ampDataPin = pin;
    int storedMic = prefs.getUChar("mic_dout", DEFAULT_MIC_DATA);
    if (storedMic == 34 || storedMic == 35) micDataPin = storedMic;
    ledDayPercent = prefs.getUChar("led_day", HOUSE_LED_DAY_PERCENT);
    ledNightPercent = prefs.getUChar("led_night", HOUSE_LED_NIGHT_PERCENT);
    if (ledDayPercent > 100) ledDayPercent = HOUSE_LED_DAY_PERCENT;
    if (ledNightPercent > 100) ledNightPercent = HOUSE_LED_NIGHT_PERCENT;
    int storedAssist = prefs.getUChar("assist_pin", 0);
    if (storedAssist) assistPin = storedAssist;
    prefs.end();
  }
  allButtonPin = roomNumber == 2 ? ROOM2_ALL_BUTTON_PIN : DEFAULT_ALL_BUTTON_PIN;
  pinMode(allButtonPin, INPUT_PULLUP);
  if (assistPin >= 0) pinMode(assistPin, INPUT_PULLUP);
  Serial.printf("ALL button GPIO%d, amplifier DIN GPIO%d, mic DOUT GPIO%d, assistant button %s%d\n",
                allButtonPin, ampDataPin, micDataPin, assistPin >= 0 ? "GPIO" : "none ", assistPin);
  snprintf(deviceName, sizeof(deviceName), "HOUSE-%s-%08lX",
           roomShortName(roomNumber), (unsigned long)nodeId);
  Serial.printf("Node %s id=%08lX room=%u\n", deviceName,
                (unsigned long)nodeId, unsigned(roomNumber));
#if HOUSE_LCD1602_DIRECT
  if (roomNumber == 1) {
    lcd.begin();
    char upper[17], lower[17];
    centerText(upper, "FLOOR 1");
    centerText(lower, "STARTING");
    lcd.showUpsideDown(upper, lower);
  }
#endif
  if (!initI2s()) {
    Serial.println("ERROR: I2S init failed; check ESP32 model and pins");
    fatalBlink();
  }
  if (!LittleFS.begin(false)) Serial.println("No alarm.raw; synthesized chime is active");
  udpTxMutex = xSemaphoreCreateMutex();
  if (!udpTxMutex) {
    Serial.println("ERROR: network mutex allocation failed");
    fatalBlink();
  }
  connectWifi();
  lastWifiAttemptMs = millis();
#if HOUSE_ENABLE_BLE
  Serial.printf("HEAP before BLE=%lu\n", (unsigned long)ESP.getFreeHeap());
  initBle();
#endif
  Serial.printf("HEAP=%lu\n", (unsigned long)ESP.getFreeHeap());
  if (xTaskCreatePinnedToCore(playbackTask, "speaker", 4096, nullptr, 5, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(captureTask, "microphone", 6144, nullptr, 4, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(netTask, "net", 4096, nullptr, 6, nullptr, 0) != pdPASS) {
    Serial.println("ERROR: audio task allocation failed");
    fatalBlink();
  }
#if HOUSE_ENABLE_BLE
  Serial.println("Ready: Wi-Fi voice intercom, BLE + web control.");
#else
  Serial.println("Ready: Wi-Fi voice intercom, web control, Wi-Fi sleep off.");
#endif
}

void loop() {
  esp_task_wdt_reset();
  updateStatusLed();
  processUsbSetup();
  monitorButtons();
  // Periodic stats are off by default: TX0 sits next to the amplifier DIN
  // (GPIO22) on the DevKit; a squeak every ~5 s on box 2F is suspected to be
  // UART bursts leaking into DIN. Use STATS / STATS ON or /stats instead.
  static uint32_t lastRxReportMs = 0;
  if (statsPeriodic && millis() - lastRxReportMs >= 5000) {
    lastRxReportMs = millis();
    Serial.print(statsText());
  }
  if (soundTestUntilMs && int32_t(millis() - soundTestUntilMs) >= 0) {
    soundTestUntilMs = 0;
    stopAlarm();
    Serial.println("TEST SOUND DONE");
  }
  if (micTestUntilMs && int32_t(millis() - micTestUntilMs) >= 0) {
    micTestUntilMs = 0;
    Serial.println("TEST MIC DONE");
  }
#if HOUSE_LCD1602_DIRECT
  updateLcd();
#endif
  // UDP sockets live in netTask; loop() only keeps Wi-Fi and the web page up.
  static uint32_t wifiDownSinceMs = 0;
  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSinceMs = 0;
    wifiRebootMarker = 0;
    if (!webStarted) startWeb();
  } else {
    if (!wifiDownSinceMs) wifiDownSinceMs = millis();
    // A Wi-Fi stack that never comes back is cured by a clean restart, but
    // only once until the network has been seen again.
    if (!wifiPassword.isEmpty() && wifiRebootMarker != WIFI_REBOOT_MAGIC &&
        millis() - wifiDownSinceMs > WIFI_REBOOT_MS) {
      Serial.println("Wi-Fi down for 10 min: restarting once");
      wifiRebootMarker = WIFI_REBOOT_MAGIC;
      delay(100);
      ESP.restart();
    }
    if (millis() - lastWifiAttemptMs > 5000) {
      lastWifiAttemptMs = millis();
      connectWifi();
    }
  }
  if (webStarted) web.handleClient();
  static uint32_t lastNightCheckMs = 0;
  if (millis() - lastNightCheckMs >= 10000) {
    lastNightCheckMs = millis();
    bool night = isNightNow();
    if (night != nightMode) Serial.printf("Night mode %s\n", night ? "ON" : "OFF");
    nightMode = night;
  }

  uint64_t now = esp_timer_get_time() / 1000ULL;
  bool trigger = false;
  portENTER_CRITICAL(&stateMux);
  if (deadlineMs && now >= deadlineMs) {
    deadlineMs = 0;
    alarmActive = true;
    trigger = true;
  }
  ControlPending todo = pending;
  pending = {0, 0};
  portEXIT_CRITICAL(&stateMux);
  if (trigger) {
    clearQueue();
    updateBleStatus("RINGING");
  }
  if (todo.type) {
    for (int i = 0; i < 3; ++i) {
      sendPacket(todo.type, todo.value);
      delay(10);
    }
  }
  delay(2);
}
