#pragma once

#include <Arduino.h>
#include <cstring>

// HD44780-compatible 16x2 in 4-bit, write-only mode. Supply the LCD logic
// from ESP32 3V3: the unknown 1602A controller is not proven 3.3 V tolerant
// at its inputs when powered from 5 V. RW must be tied to GND.
class Lcd1602Direct {
 public:
  static constexpr uint8_t RS = 16;
  static constexpr uint8_t E = 17;
  static constexpr uint8_t D4 = 23;
  static constexpr uint8_t D5 = 13;
  static constexpr uint8_t D6 = 4;
  static constexpr uint8_t D7 = 5;

  void begin() {
    const uint8_t pins[] = {RS, E, D4, D5, D6, D7};
    for (uint8_t pin : pins) {
      digitalWrite(pin, LOW);
      pinMode(pin, OUTPUT);
    }
    delay(50);
    sendNibble(0x03);
    delay(5);
    sendNibble(0x03);
    delayMicroseconds(200);
    sendNibble(0x03);
    sendNibble(0x02); // 4-bit mode
    command(0x28);   // two lines, 5x8 characters
    command(0x08);   // display off while clearing
    command(0x01);
    delay(2);
    command(0x06);   // cursor advances
    command(0x0C);   // display on, no visible cursor
    ready_ = true;
  }

  void show(const char *top, const char *bottom) {
    if (!ready_) return;
    rotatedValid_ = false;
    writeLine(0, top);
    writeLine(1, bottom);
  }

  // A 1602 cannot rotate its built-in font. For the upside-down first box,
  // characters that look the same turned 180 degrees come from the ROM font
  // (see romForRotated); every other character needs one of the eight
  // custom glyph slots, so each screen may use at most eight such letters.
  void showUpsideDown(const char *top, const char *bottom) {
    if (!ready_) return;
    char visual[2][16];
    padLine(visual[0], top);
    padLine(visual[1], bottom);
    if (rotatedValid_ && memcmp(visual, rotatedOld_, sizeof(visual)) == 0) return;

    char symbols[8] = {};
    uint8_t symbolCount = 0;
    for (const auto &line : visual) {
      for (char c : line) {
        if (c == ' ' || romForRotated(c)) continue;
        bool known = false;
        for (uint8_t i = 0; i < symbolCount; ++i) known |= symbols[i] == c;
        if (!known && symbolCount < 8 && glyphFor(c)) symbols[symbolCount++] = c;
      }
    }

    command(0x08); // hide brief CGRAM remapping, then restore the display
    for (uint8_t slot = 0; slot < symbolCount; ++slot) {
      const uint8_t *glyph = glyphFor(symbols[slot]);
      command(0x40 | (slot << 3));
      // Flip rows 0..6 only and keep row 7 (cursor line) empty, so the drawn
      // letters sit at the same height as the unrotated ROM letters.
      for (uint8_t y = 0; y < 8; ++y) {
        uint8_t original = y < 7 ? glyph[6 - y] : 0;
        uint8_t rotated = 0;
        for (uint8_t x = 0; x < 5; ++x)
          if (original & (1U << x)) rotated |= uint8_t(1U << (4 - x));
        sendByte(rotated, true);
      }
    }

    // A physical 180-degree turn reverses both row order and column order.
    writeRotatedLine(0, visual[1], symbols, symbolCount);
    writeRotatedLine(1, visual[0], symbols, symbolCount);
    command(0x0C);
    memcpy(rotatedOld_, visual, sizeof(visual));
    rotatedValid_ = true;
    memset(old_, 0, sizeof(old_));
  }

 private:
  bool ready_ = false;
  char old_[2][17] = {{0}, {0}};
  char rotatedOld_[2][16] = {{0}, {0}};
  bool rotatedValid_ = false;

  static void padLine(char (&line)[16], const char *source) {
    memset(line, ' ', 16);
    for (uint8_t i = 0; i < 16 && source[i]; ++i) line[i] = source[i];
  }

  // HD44780 ROM characters whose 5x7 bitmap turned 180 degrees is the same
  // letter (O, N, S, ...) or another one (M <-> W, 6 <-> 9). Zero: none.
  static char romForRotated(char c) {
    switch (c) {
      case 'O': case 'H': case 'I': case 'N': case 'S': case 'X': case 'Z':
      case '0': case '8': case '-': case ':': case '+': case '=': case '*':
        return c;
      case 'M': return 'W';
      case 'W': return 'M';
      case '6': return '9';
      case '9': return '6';
      default: return 0;
    }
  }

  static const uint8_t *glyphFor(char c) {
    struct Glyph { char name; uint8_t rows[8]; };
    static const Glyph font[] = {
      {'-', {0, 0, 0, 31, 0, 0, 0, 0}},
      {':', {0, 4, 4, 0, 4, 4, 0, 0}},
      {'0', {14, 17, 19, 21, 25, 17, 14, 0}},
      {'1', {4, 12, 4, 4, 4, 4, 14, 0}},
      {'2', {14, 17, 1, 2, 4, 8, 31, 0}},
      {'3', {30, 1, 1, 14, 1, 1, 30, 0}},
      {'4', {2, 6, 10, 18, 31, 2, 2, 0}},
      {'5', {31, 16, 16, 30, 1, 1, 30, 0}},
      {'6', {14, 16, 16, 30, 17, 17, 14, 0}},
      {'7', {31, 1, 2, 4, 8, 8, 8, 0}},
      {'8', {14, 17, 17, 14, 17, 17, 14, 0}},
      {'9', {14, 17, 17, 15, 1, 1, 14, 0}},
      {'A', {14, 17, 17, 31, 17, 17, 17, 0}},
      {'B', {30, 17, 17, 30, 17, 17, 30, 0}},
      {'C', {14, 17, 16, 16, 16, 17, 14, 0}},
      {'D', {30, 17, 17, 17, 17, 17, 30, 0}},
      {'E', {31, 16, 16, 30, 16, 16, 31, 0}},
      {'F', {31, 16, 16, 30, 16, 16, 16, 0}},
      {'G', {14, 17, 16, 23, 17, 17, 15, 0}},
      {'I', {14, 4, 4, 4, 4, 4, 14, 0}},
      {'K', {17, 18, 20, 24, 20, 18, 17, 0}},
      {'L', {16, 16, 16, 16, 16, 16, 31, 0}},
      {'M', {17, 27, 21, 21, 17, 17, 17, 0}},
      {'N', {17, 25, 21, 19, 17, 17, 17, 0}},
      {'O', {14, 17, 17, 17, 17, 17, 14, 0}},
      {'P', {30, 17, 17, 30, 16, 16, 16, 0}},
      {'R', {30, 17, 17, 30, 20, 18, 17, 0}},
      {'T', {31, 4, 4, 4, 4, 4, 4, 0}},
      {'U', {17, 17, 17, 17, 17, 17, 14, 0}},
      {'V', {17, 17, 17, 17, 17, 10, 4, 0}},
      {'W', {17, 17, 17, 21, 21, 21, 10, 0}},
      {'Y', {17, 17, 10, 4, 4, 4, 4, 0}},
    };
    for (const auto &glyph : font) if (glyph.name == c) return glyph.rows;
    return nullptr;
  }

  void writeRotatedLine(uint8_t row, const char (&visual)[16],
                        const char (&symbols)[8], uint8_t count) {
    command(row ? 0xC0 : 0x80);
    for (int8_t physical = 0; physical < 16; ++physical) {
      char c = visual[15 - physical];
      uint8_t code = ' ';
      if (char rom = romForRotated(c)) {
        code = uint8_t(rom);
      } else if (c != ' ') {
        for (uint8_t slot = 0; slot < count; ++slot)
          if (symbols[slot] == c) { code = slot; break; }
      }
      sendByte(code, true);
    }
  }

  void sendNibble(uint8_t value) {
    digitalWrite(D4, value & 0x01);
    digitalWrite(D5, value & 0x02);
    digitalWrite(D6, value & 0x04);
    digitalWrite(D7, value & 0x08);
    delayMicroseconds(1);
    digitalWrite(E, HIGH);
    delayMicroseconds(2);
    digitalWrite(E, LOW);
    delayMicroseconds(50);
  }

  void sendByte(uint8_t value, bool data) {
    digitalWrite(RS, data ? HIGH : LOW);
    sendNibble(value >> 4);
    sendNibble(value & 0x0f);
  }

  void command(uint8_t value) {
    sendByte(value, false);
    if (value == 0x01 || value == 0x02) delay(2);
  }

  void writeLine(uint8_t row, const char *source) {
    char line[17];
    memset(line, ' ', 16);
    line[16] = '\0';
    for (uint8_t i = 0; i < 16 && source[i]; ++i) line[i] = source[i];
    if (memcmp(line, old_[row], 16) == 0) return;
    command(row ? 0xC0 : 0x80);
    for (uint8_t i = 0; i < 16; ++i) sendByte(line[i], true);
    memcpy(old_[row], line, sizeof(line));
  }
};
