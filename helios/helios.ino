#include <Wire.h>
#include <EEPROM.h>

const uint8_t PIN_ENC_CLK      = 2;   // D2
const uint8_t PIN_ENC_DT       = 3;   // D3
const uint8_t PIN_ENC_SW       = 4;   // D4, active LOW (pull-up on board)
const uint8_t PIN_BTN_TIME     = 5;   // D5, active HIGH (pull-down on board)
const uint8_t PIN_BTN_ALARM    = 6;   // D6, active HIGH
const uint8_t PIN_BTN_MANUAL   = 7;   // D7, active HIGH
const uint8_t PIN_RELAY        = 8;   // D8 -> relay module signal
const uint8_t PIN_SPI_DATA     = 9;   // D9  -> U1 SER (chained into U4)
const uint8_t PIN_SPI_CLK      = 10;  // D10 -> both 595 SRCLK
const uint8_t PIN_LATCH_TIME   = 11;  // D11 -> U1 RCLK
const uint8_t PIN_LATCH_ALARM  = 12;  // D12 -> U4 RCLK
const uint8_t PIN_SEL_A        = 13;  // D13 -> 138 A0
const uint8_t PIN_SEL_B        = A0;  //     -> 138 A1
const uint8_t PIN_SEL_C        = A1;  //     -> 138 A2
const uint8_t PIN_EN_TIME      = A2;  // 74HC138 enable, ACTIVE LOW
const uint8_t PIN_EN_ALARM     = A3;  // 74HC138 enable, ACTIVE LOW

#define RELAY_ACTIVE_HIGH   1     // 0 if relay module is active-low 
#define SEGMENTS_ACTIVE_LOW 1     // PNP high-side digit drive -> segments sink (LOW = on)
#define ENCODER_REVERSED    1     // changes encoder's clockwise or counter-clockwise direction
#define ALARM_ON_MINUTES    60UL  // how long the lamp stays on after the alarm fires
#define MUX_INTERVAL_US     2000  // per-digit multiplex period (4 digits -> 125 Hz refresh)
#define BLANK_SETTLE_US     100   // digit-off settle time before lighting (for anti-ghosting)
#define BLINK_PERIOD_MS     200   // edit-mode blink rate
#define DEBOUNCE_MS         50    // press must be stable this long to count
#define EDIT_TIMEOUT_MS     20000 // auto-leave edit mode after this much inactivity
#define DEBUG_SERIAL        0     // FOR DEBUGGING if u wanna test the button functions on the serial monitor

const uint8_t DS3231_ADDR   = 0x68;
const int     EE_MAGIC_ADDR = 0;
const int     EE_ALARM_H    = 1;
const int     EE_ALARM_M    = 2;
const uint8_t EE_MAGIC      = 0xA5;

// 7-segment patterns
const uint8_t DIGIT_FONT[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
const uint8_t SEG_BLANK = 0x00;
const uint8_t SEG_DP    = 0x80;

// state
enum Mode : uint8_t {
  MODE_RUN,
  MODE_SET_TIME_H,  MODE_SET_TIME_M,
  MODE_SET_ALARM_H, MODE_SET_ALARM_M
};

Mode     mode         = MODE_RUN;
bool     systemOn     = true;
bool     manualOn     = false;   // manual override: relay forced on + alarm disabled
bool     alarmRinging = false;   // alarm fired, relay on until timeout/cancel
uint32_t alarmStartedMs = 0;
uint32_t lastEditActivityMs = 0; // for the edit-mode inactivity timeout

uint8_t timeH = 0, timeM = 0, timeS = 0;   // current RTC time
uint8_t alarmH = 7, alarmM = 0;            // alarm setpoint
uint8_t editH, editM;                      // scratch values while editing
int8_t  lastAlarmTriggerMin = -1;          // minute-of-day guard, avoids retrigger

// display buffers: segment pattern per digit, index 0 = leftmost
uint8_t timeBuf[4];
uint8_t alarmBuf[4];

// DS3231
uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

bool rtcReadTime() {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)3) != 3) return false;
  timeS = bcd2dec(Wire.read() & 0x7F);
  timeM = bcd2dec(Wire.read() & 0x7F);
  timeH = bcd2dec(Wire.read() & 0x3F);   // 24h
  return true;
}

void rtcSetTime(uint8_t h, uint8_t m) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  Wire.write(dec2bcd(0));        // seconds
  Wire.write(dec2bcd(m));
  Wire.write(dec2bcd(h));        // bit6 clear = 24h mode
  Wire.endTransmission();

  // clear the oscillator-stop flag so the time is marked valid
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x0F);
  Wire.endTransmission();
  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)1) == 1) {
    uint8_t status = Wire.read();
    Wire.beginTransmission(DS3231_ADDR);
    Wire.write(0x0F);
    Wire.write(status & ~0x80);
    Wire.endTransmission();
  }
}

// alarm persistence
void loadAlarm() {
  if (EEPROM.read(EE_MAGIC_ADDR) == EE_MAGIC) {
    alarmH = EEPROM.read(EE_ALARM_H) % 24;
    alarmM = EEPROM.read(EE_ALARM_M) % 60;
  }
}

void saveAlarm() {
  EEPROM.update(EE_MAGIC_ADDR, EE_MAGIC);
  EEPROM.update(EE_ALARM_H, alarmH);
  EEPROM.update(EE_ALARM_M, alarmM);
}

// relay
void relayWrite(bool on) {
#if RELAY_ACTIVE_HIGH
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
#else
  digitalWrite(PIN_RELAY, on ? LOW : HIGH);
#endif
}

void updateRelay() {
  relayWrite(systemOn && (manualOn || alarmRinging));
}

// encoder
int8_t pollEncoder() {
  static uint8_t lastClk = HIGH;
  static uint32_t lastStepMs = 0;
  int8_t step = 0;

  uint8_t clk = digitalRead(PIN_ENC_CLK);
  if (lastClk == HIGH && clk == LOW && (millis() - lastStepMs) > 3) {
    step = digitalRead(PIN_ENC_DT) ? +1 : -1;
#if ENCODER_REVERSED
    step = -step;
#endif
    lastStepMs = millis();
  }
  lastClk = clk;
  return step;
}

// buttons
struct Button {
  uint8_t  pin;
  bool     activeHigh;
  bool     stable;
  bool     lastRaw;
  uint32_t lastChangeMs;
  bool     pressedEvent;
};

Button buttons[] = {
  { PIN_BTN_TIME,   true,  false, false, 0, false },
  { PIN_BTN_ALARM,  true,  false, false, 0, false },
  { PIN_BTN_MANUAL, true,  false, false, 0, false },
  { PIN_ENC_SW,     false, false, false, 0, false },
};
enum { BTN_TIME, BTN_ALARM, BTN_MANUAL, BTN_POWER };

void scanButtons() {
  uint32_t now = millis();
  for (Button &b : buttons) {
    bool raw = digitalRead(b.pin);
    if (!b.activeHigh) raw = !raw;
    if (raw != b.lastRaw) {
      b.lastRaw = raw;
      b.lastChangeMs = now;
    }
    if ((now - b.lastChangeMs) >= DEBOUNCE_MS && raw != b.stable) {
      b.stable = raw;
      if (raw) b.pressedEvent = true;   // rising edge = press
    }
  }
}

bool pressed(uint8_t idx) {
  if (buttons[idx].pressedEvent) {
    buttons[idx].pressedEvent = false;
    return true;
  }
  return false;
}

// display rendering
void renderPair(uint8_t *buf, uint8_t h, uint8_t m, bool hideH, bool hideM) {
  buf[0] = hideH ? SEG_BLANK : DIGIT_FONT[h / 10];
  buf[1] = hideH ? SEG_BLANK : DIGIT_FONT[h % 10];
  buf[2] = hideM ? SEG_BLANK : DIGIT_FONT[m / 10];
  buf[3] = hideM ? SEG_BLANK : DIGIT_FONT[m % 10];
}

void renderDisplays() {
  bool blinkOff = (millis() / BLINK_PERIOD_MS) & 1;

  // TIME
  if (mode == MODE_SET_TIME_H || mode == MODE_SET_TIME_M) {
    renderPair(timeBuf, editH, editM,
               mode == MODE_SET_TIME_H && blinkOff,
               mode == MODE_SET_TIME_M && blinkOff);
  } else {
    renderPair(timeBuf, timeH, timeM, false, false);
    if (timeS & 1) timeBuf[1] |= SEG_DP;   // 1 Hz dot blink
  }

  // ALARM
  if (mode == MODE_SET_ALARM_H || mode == MODE_SET_ALARM_M) {
    renderPair(alarmBuf, editH, editM,
               mode == MODE_SET_ALARM_H && blinkOff,
               mode == MODE_SET_ALARM_M && blinkOff);
  } else {
    renderPair(alarmBuf, alarmH, alarmM, false, false);
  }
  alarmBuf[1] |= SEG_DP;                     // hh.mm separator, always on
  if (!manualOn) alarmBuf[3] |= SEG_DP;      // armed indicator
}

// shift one digit's pattern into both 595s and latch
void shiftBoth(uint8_t alarmByte, uint8_t timeByte) {
#if SEGMENTS_ACTIVE_LOW
  alarmByte = ~alarmByte;
  timeByte  = ~timeByte;
#endif
  shiftOut(PIN_SPI_DATA, PIN_SPI_CLK, MSBFIRST, alarmByte);
  shiftOut(PIN_SPI_DATA, PIN_SPI_CLK, MSBFIRST, timeByte);
  digitalWrite(PIN_LATCH_TIME, HIGH);
  digitalWrite(PIN_LATCH_ALARM, HIGH);
  digitalWrite(PIN_LATCH_TIME, LOW);
  digitalWrite(PIN_LATCH_ALARM, LOW);
}

void muxStep() {
  static uint8_t phase = 0;

  digitalWrite(PIN_EN_TIME,  HIGH);
  digitalWrite(PIN_EN_ALARM, HIGH);

  shiftBoth(SEG_BLANK, SEG_BLANK);          // drain the segment lines while blanked

  digitalWrite(PIN_SEL_A, phase & 1);
  digitalWrite(PIN_SEL_B, (phase >> 1) & 1);
  digitalWrite(PIN_SEL_C, LOW);

  shiftBoth(alarmBuf[phase], timeBuf[phase]);

  if (systemOn) {
    delayMicroseconds(BLANK_SETTLE_US);     // let PNPs/segment lines settle first
    digitalWrite(PIN_EN_TIME,  LOW);
    digitalWrite(PIN_EN_ALARM, LOW);
  }

  phase = (phase + 1) & 3;
}

// alarm logic
void checkAlarm() {
  int16_t nowMin = timeH * 60 + timeM;

  if (systemOn && !manualOn && mode == MODE_RUN &&
      timeH == alarmH && timeM == alarmM && nowMin != lastAlarmTriggerMin) {
    lastAlarmTriggerMin = nowMin;
    alarmRinging = true;
    alarmStartedMs = millis();
  }
  if (nowMin != lastAlarmTriggerMin) lastAlarmTriggerMin = -1;

  if (alarmRinging && (millis() - alarmStartedMs) >= ALARM_ON_MINUTES * 60000UL) {
    alarmRinging = false;
  }
}

// input handling / state machine
bool inEditMode() { return mode != MODE_RUN; }

void applyEncoderToEdit(int8_t d) {
  if (d == 0) return;
  lastEditActivityMs = millis();
  switch (mode) {
    case MODE_SET_TIME_H:
    case MODE_SET_ALARM_H:
      editH = (uint8_t)((editH + 24 + (d % 24)) % 24);
      break;
    case MODE_SET_TIME_M:
    case MODE_SET_ALARM_M:
      editM = (uint8_t)((editM + 60 + (d % 60)) % 60);
      break;
    default:
      break;
  }
}

void handleInputs() {
  int8_t enc = pollEncoder();

  // power toggle (encoder push)
  // ignored while editing so an accidental knob press mid-edit can't switch the whole system off
  if (pressed(BTN_POWER) && !inEditMode()) {
#if DEBUG_SERIAL
    Serial.println(F("BTN_POWER (enc)"));
#endif
    systemOn = !systemOn;
    if (!systemOn) {
      mode = MODE_RUN;
      manualOn = false;
      alarmRinging = false;
    }
  }

  if (!systemOn) return;

  if (pressed(BTN_TIME)) {
#if DEBUG_SERIAL
    Serial.println(F("BTN_TIME (D5)"));
#endif
    lastEditActivityMs = millis();
    switch (mode) {
      case MODE_RUN:        editH = timeH; editM = timeM; mode = MODE_SET_TIME_H; break;
      case MODE_SET_TIME_H: mode = MODE_SET_TIME_M; break;
      case MODE_SET_TIME_M:
        rtcSetTime(editH, editM);
        timeH = editH; timeM = editM; timeS = 0;
        mode = MODE_RUN;
        break;
      default: break;   // ignore while editing the alarm
    }
  }

  if (pressed(BTN_ALARM)) {
#if DEBUG_SERIAL
    Serial.println(F("BTN_ALARM (D6)"));
#endif
    lastEditActivityMs = millis();
    switch (mode) {
      case MODE_RUN:         editH = alarmH; editM = alarmM; mode = MODE_SET_ALARM_H; break;
      case MODE_SET_ALARM_H: mode = MODE_SET_ALARM_M; break;
      case MODE_SET_ALARM_M:
        alarmH = editH; alarmM = editM;
        saveAlarm();
        mode = MODE_RUN;
        break;
      default: break;   // ignore while editing the time
    }
  }

  if (pressed(BTN_MANUAL)) {
#if DEBUG_SERIAL
    Serial.println(F("BTN_MANUAL (D7)"));
#endif
    if (alarmRinging) {
      alarmRinging = false;        // first press just cancels the alarm
    } else {
      manualOn = !manualOn;        // override on = relay on, alarm disabled
      if (manualOn) alarmRinging = false;
    }
  }

  // encoder only acts in edit modes
  if (inEditMode()) {
    applyEncoderToEdit(enc);
    if (millis() - lastEditActivityMs > EDIT_TIMEOUT_MS) mode = MODE_RUN;
  }
}

// setup
void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
  Serial.println(F("HELIOS debug"));
#endif

  pinMode(PIN_ENC_CLK,     INPUT);
  pinMode(PIN_ENC_DT,      INPUT);
  pinMode(PIN_ENC_SW,      INPUT);
  pinMode(PIN_BTN_TIME,    INPUT);
  pinMode(PIN_BTN_ALARM,   INPUT);
  pinMode(PIN_BTN_MANUAL,  INPUT);

  pinMode(PIN_RELAY,       OUTPUT);
  pinMode(PIN_SPI_DATA,    OUTPUT);
  pinMode(PIN_SPI_CLK,     OUTPUT);
  pinMode(PIN_LATCH_TIME,  OUTPUT);
  pinMode(PIN_LATCH_ALARM, OUTPUT);
  pinMode(PIN_SEL_A,       OUTPUT);
  pinMode(PIN_SEL_B,       OUTPUT);
  pinMode(PIN_SEL_C,       OUTPUT);
  pinMode(PIN_EN_TIME,     OUTPUT);
  pinMode(PIN_EN_ALARM,    OUTPUT);

  relayWrite(false);
  digitalWrite(PIN_EN_TIME,  HIGH);   // displays off until first mux step
  digitalWrite(PIN_EN_ALARM, HIGH);
  digitalWrite(PIN_LATCH_TIME,  LOW);
  digitalWrite(PIN_LATCH_ALARM, LOW);

  Wire.begin();
  loadAlarm();
  rtcReadTime();
}

// loop
void loop() {
  static uint32_t lastMuxUs  = 0;
  static uint32_t lastRtcMs  = 0;

  uint32_t nowUs = micros();
  if (nowUs - lastMuxUs >= MUX_INTERVAL_US) {
    lastMuxUs = nowUs;
    muxStep();
  }

  if (millis() - lastRtcMs >= 200) {
    lastRtcMs = millis();
    rtcReadTime();
    checkAlarm();
  }

  scanButtons();
  handleInputs();
  renderDisplays();
  updateRelay();
}
