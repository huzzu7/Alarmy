#include <Wire.h>
#include <EEPROM.h>
#include "Waveshare_LCD1602_RGB.h"
#include <DS1302.h>

// ---------------- Pins ----------------
const int pinA = 2;
const int pinB = 3;
const int buttonPin = A0;
const int rotButtonPin = 4;
const int buzzer = 8;
const int emitterPin = 9;
const int sensorPin = A1;
const int motorPin = 5;

// ---------------- DS1302 pins ----------------
const int kCePin   = 10;
const int kIoPin   = 11;
const int kSclkPin = 12;

DS1302 rtc(kCePin, kIoPin, kSclkPin);

// ---------------- LCD ----------------
Waveshare_LCD1602_RGB lcd(16, 2);

// ---------------- Custom underlined weekday chars ----------------
// slots:
// 0 = M
// 1 = T
// 2 = W
// 3 = F
// 4 = S
byte UL_M[8] = {
  B10001,
  B11011,
  B10101,
  B10001,
  B10001,
  B10001,
  B00000,
  B11111
};

byte UL_T[8] = {
  B11111,
  B00100,
  B00100,
  B00100,
  B00100,
  B00100,
  B00000,
  B11111
};

byte UL_W[8] = {
  B10001,
  B10001,
  B10001,
  B10101,
  B10101,
  B01010,
  B00000,
  B11111
};

byte UL_F[8] = {
  B11111,
  B10000,
  B10000,
  B11110,
  B10000,
  B10000,
  B00000,
  B11111
};

byte UL_S[8] = {
  B01111,
  B10000,
  B10000,
  B01110,
  B00001,
  B00001,
  B11110,
  B11111
};

// ---------------- Modes ----------------
enum UiMode {
  MODE_NORMAL,
  MODE_SET_TIME,
  MODE_SET_ALARM
};

UiMode mode = MODE_NORMAL;

// ---------------- Current time/date ----------------
int currentHour = 14;
int currentMinute = 30;
int currentDay = Time::kSunday;
int currentMonth = 3;
int currentDate = 15;
int currentYear = 2026;

// ---------------- Alarm ----------------
int alarmHour = 7;
int alarmMinute = 0;

// bit0=Mon, bit1=Tue, ..., bit6=Sun
uint8_t alarmDaysMask = 0x7F;

// ---------------- Photodiode sensing ----------------
int baselineSignal = 0;
const int buffer = 50;
const int detectionThreshold = 100;
bool proxDetected = false;
unsigned long lastProximitySample = 0;
const unsigned long proximitySamplePeriodMs = 15;

// ---------------- EEPROM layout ----------------
struct AlarmDataV2 {
  uint8_t magic;
  uint8_t hour;
  uint8_t minute;
  uint8_t daysMask;
};

struct AlarmDataV1 {
  uint8_t magic;
  uint8_t hour;
  uint8_t minute;
};

const uint8_t ALARM_MAGIC_V2 = 0xA8;
const uint8_t ALARM_MAGIC_V1 = 0xA7;
const int EEPROM_ADDR_ALARM = 0;

// ---------------- Button state ----------------
int lastButtonState = HIGH;
int lastRotButtonState = HIGH;
unsigned long lastButtonTime = 0;
unsigned long lastRotButtonTime = 0;
const unsigned long buttonDebounceMs = 30;

// ---------------- Encoder state ----------------
volatile int8_t encoderAcc = 0;
volatile uint8_t encoderState = 0;

const int8_t transitionTable[16] = {
  0, +1, -1,  0,
  -1,  0,  0, +1,
  +1,  0,  0, -1,
   0, -1, +1,  0
};

// ---------------- UI state ----------------
// MODE_SET_TIME:
// 0 = hours, 1 = minutes, 2 = AM/PM, 3 = day, 4 = month, 5 = date
//
// MODE_SET_ALARM:
// 0 = hours, 1 = minutes, 2 = AM/PM,
// 3 = Mon, 4 = Tue, 5 = Wed, 6 = Thu, 7 = Fri, 8 = Sat, 9 = Sun
int editIndex = 0;

// ---------------- LCD refresh control ----------------
bool displayDirty = true;
UiMode lastRenderedMode = MODE_SET_ALARM;
unsigned long lastLcdUpdate = 0;
const unsigned long lcdUpdatePeriod = 50;

// ---------------- Software blink ----------------
bool blinkVisible = true;
unsigned long lastBlinkToggle = 0;
const unsigned long blinkPeriod = 400;

// ---------------- LCD line cache ----------------
char lastLine1[17] = "                ";
char lastLine2[17] = "                ";

// ---------------- RTC sync timing ----------------
unsigned long lastRtcPoll = 0;
const unsigned long rtcPollPeriodMs = 250;

// ---------------- Helpers ----------------
void fillLine(char *line) {
  for (int i = 0; i < 16; i++) line[i] = ' ';
  line[16] = '\0';
}

bool fallingEdgeDebounced(int pin, int &lastState, unsigned long &lastTimeMs) {
  int currentState = digitalRead(pin);
  bool fell = false;

  if (lastState == HIGH && currentState == LOW) {
    unsigned long now = millis();
    if (now - lastTimeMs >= buttonDebounceMs) {
      fell = true;
      lastTimeMs = now;
    }
  }

  lastState = currentState;
  return fell;
}

int getDisplayHour12From(int hour24) {
  int h = hour24 % 12;
  if (h == 0) h = 12;
  return h;
}

bool isPMFrom(int hour24) {
  return hour24 >= 12;
}

void setFromDisplayHour12AndPeriod(int displayHour12, bool pm, int &hour24) {
  if (displayHour12 == 12) {
    hour24 = pm ? 12 : 0;
  } else {
    hour24 = pm ? (displayHour12 + 12) : displayHour12;
  }
}

const char* dayShort(uint8_t day) {
  switch (day) {
    case Time::kSunday:    return "Sun";
    case Time::kMonday:    return "Mon";
    case Time::kTuesday:   return "Tue";
    case Time::kWednesday: return "Wed";
    case Time::kThursday:  return "Thu";
    case Time::kFriday:    return "Fri";
    case Time::kSaturday:  return "Sat";
  }
  return "???";
}

const char* monthShort(uint8_t mon) {
  switch (mon) {
    case 1:  return "Jan";
    case 2:  return "Feb";
    case 3:  return "Mar";
    case 4:  return "Apr";
    case 5:  return "May";
    case 6:  return "Jun";
    case 7:  return "Jul";
    case 8:  return "Aug";
    case 9:  return "Sep";
    case 10: return "Oct";
    case 11: return "Nov";
    case 12: return "Dec";
  }
  return "???";
}

int daysInMonth(int year, int month) {
  switch (month) {
    case 1:  return 31;
    case 2: {
      bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
      return leap ? 29 : 28;
    }
    case 3:  return 31;
    case 4:  return 30;
    case 5:  return 31;
    case 6:  return 30;
    case 7:  return 31;
    case 8:  return 31;
    case 9:  return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
  }
  return 31;
}

void clampCurrentDateToMonth() {
  int dim = daysInMonth(currentYear, currentMonth);
  if (currentDate > dim) currentDate = dim;
  if (currentDate < 1) currentDate = 1;
}

bool alarmDayEnabled(int dayIdxMon0ToSun6) {
  return (alarmDaysMask & (1 << dayIdxMon0ToSun6)) != 0;
}

void setAlarmDayEnabled(int dayIdxMon0ToSun6, bool enabled) {
  if (enabled) alarmDaysMask |= (1 << dayIdxMon0ToSun6);
  else         alarmDaysMask &= ~(1 << dayIdxMon0ToSun6);
  displayDirty = true;
}

char normalDayLetter(int dayIdxMon0ToSun6) {
  const char letters[7] = {'M', 'T', 'W', 'T', 'F', 'S', 'S'};
  return letters[dayIdxMon0ToSun6];
}

uint8_t underlinedCharForDay(int dayIdxMon0ToSun6) {
  switch (dayIdxMon0ToSun6) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 1;
    case 4: return 3;
    case 5: return 4;
    case 6: return 4;
  }
  return 0;
}

//////////////////////////
// BUZZER SOUNDS
//////////////////////////

bool alarmRinging = false;
bool alarmLatched = false;  // prevents retrigger within same minute

// non-blocking Casio alarm state
unsigned long alarmToneStartedAt = 0;
unsigned long alarmPhaseStartedAt = 0;
int alarmPhase = 0;
int alarmCycleCount = 0;

void calibratePhotodiodeBaseline() {
  Serial.println("Calibrating baseline... Keep sensor clear!");
  long total = 0;

  for (int i = 0; i < 10; i++) {
    digitalWrite(emitterPin, LOW);
    delay(10);
    int offVal = analogRead(sensorPin);

    digitalWrite(emitterPin, HIGH);
    delay(1);
    int onVal = analogRead(sensorPin);

    total += abs(onVal - offVal);
  }

  baselineSignal = total / 10;
  Serial.print("Baseline Leakage detected at: ");
  Serial.println(baselineSignal);
  digitalWrite(emitterPin, LOW);
}

bool readProximityDetected() {
  unsigned long now = millis();
  if (now - lastProximitySample < proximitySamplePeriodMs) {
    return proxDetected;
  }
  lastProximitySample = now;

  digitalWrite(emitterPin, LOW);
  delayMicroseconds(9000);
  int ambientVal = analogRead(sensorPin);

  digitalWrite(emitterPin, HIGH);
  delayMicroseconds(1000);
  int activeVal = analogRead(sensorPin);

  int currentSignal = abs(activeVal - ambientVal);
  int triggerLevel = baselineSignal + buffer;
  if (triggerLevel < detectionThreshold) triggerLevel = detectionThreshold;

  proxDetected = (currentSignal > triggerLevel);

  digitalWrite(emitterPin, LOW);
  return proxDetected;
}

void startCasioAlarm() {
  alarmRinging = true;
  proxDetected = false;
  alarmPhase = 0;
  alarmCycleCount = 0;
  alarmPhaseStartedAt = millis();
  alarmToneStartedAt = 0;
}

void stopCasioAlarm() {
  alarmRinging = false;
  proxDetected = false;
  alarmPhase = 0;
  alarmCycleCount = 0;
  noTone(buzzer);
  digitalWrite(emitterPin, LOW);
}

void serviceCasioAlarm() {
  unsigned long now = millis();

  switch (alarmPhase) {
    case 0: // first beep on (100 ms)
      if (alarmToneStartedAt == 0) {
        tone(buzzer, 4096);
        alarmToneStartedAt = now;
        alarmPhaseStartedAt = now;
      }
      if (now - alarmPhaseStartedAt >= 100) {
        noTone(buzzer);
        alarmPhase = 1;
        alarmPhaseStartedAt = now;
        alarmToneStartedAt = 0;
      }
      break;

    case 1: // gap 50 ms
      if (now - alarmPhaseStartedAt >= 50) {
        alarmPhase = 2;
        alarmPhaseStartedAt = now;
      }
      break;

    case 2: // second beep on (100 ms)
      if (alarmToneStartedAt == 0) {
        tone(buzzer, 4096);
        alarmToneStartedAt = now;
        alarmPhaseStartedAt = now;
      }
      if (now - alarmPhaseStartedAt >= 100) {
        noTone(buzzer);
        alarmPhase = 3;
        alarmPhaseStartedAt = now;
        alarmToneStartedAt = 0;
      }
      break;

    case 3: // gap 200 ms to complete the original 300 ms delay after second beep
      if (now - alarmPhaseStartedAt >= 200) {
        alarmCycleCount++;
        if (alarmCycleCount >= 4) {
          alarmCycleCount = 0;
        }
        alarmPhase = 0;
        alarmPhaseStartedAt = now;
      }
      break;
  }
}

void glassClick() {
  tone(buzzer, 3200, 12);
}

void rotaryCW() {
  tone(buzzer, 2600, 7);
}

void rotaryCCW() {
  tone(buzzer, 2000, 7);
}

void successMini() {
  tone(buzzer, 1700, 30);
  delay(35);
  tone(buzzer, 2300, 40);
}

void loadAlarmFromEEPROM() {
  AlarmDataV2 dataV2;
  EEPROM.get(EEPROM_ADDR_ALARM, dataV2);

  if (dataV2.magic == ALARM_MAGIC_V2 &&
      dataV2.hour < 24 &&
      dataV2.minute < 60 &&
      (dataV2.daysMask & 0x80) == 0) {
    alarmHour = dataV2.hour;
    alarmMinute = dataV2.minute;
    alarmDaysMask = dataV2.daysMask;
    if (alarmDaysMask == 0) alarmDaysMask = 0x7F;
    return;
  }

  AlarmDataV1 dataV1;
  EEPROM.get(EEPROM_ADDR_ALARM, dataV1);

  if (dataV1.magic == ALARM_MAGIC_V1 &&
      dataV1.hour < 24 &&
      dataV1.minute < 60) {
    alarmHour = dataV1.hour;
    alarmMinute = dataV1.minute;
    alarmDaysMask = 0x7F;
    return;
  }

  alarmHour = 7;
  alarmMinute = 0;
  alarmDaysMask = 0x7F;
}

void saveAlarmToEEPROM() {
  AlarmDataV2 data;
  data.magic = ALARM_MAGIC_V2;
  data.hour = (uint8_t)alarmHour;
  data.minute = (uint8_t)alarmMinute;
  data.daysMask = alarmDaysMask & 0x7F;
  EEPROM.put(EEPROM_ADDR_ALARM, data);
}

void setBacklightForMode() {
  lcd.setRGB(255, 100, 20);
}

void updateLineIfChanged(int row, const char *newText, char *oldText) {
  for (int i = 0; i < 16; i++) {
    if (newText[i] != oldText[i]) {
      lcd.setCursor(i, row);
      char c[2] = { newText[i], '\0' };
      lcd.send_string(c);
      oldText[i] = newText[i];
    }
  }
  oldText[16] = '\0';
}

void clearLineCaches() {
  for (int i = 0; i < 16; i++) {
    lastLine1[i] = 0x7F;
    lastLine2[i] = 0x7F;
  }
  lastLine1[16] = '\0';
  lastLine2[16] = '\0';
}

void applyFieldDeltaTo(int idx, int delta, int &hour24, int &minuteVal) {
  if (delta == 0) return;

  int displayHour = getDisplayHour12From(hour24);
  bool pm = isPMFrom(hour24);

  switch (idx) {
    case 0:
      displayHour += delta;
      while (displayHour > 12) displayHour -= 12;
      while (displayHour < 1)  displayHour += 12;
      setFromDisplayHour12AndPeriod(displayHour, pm, hour24);
      break;

    case 1:
      minuteVal += delta;
      while (minuteVal > 59) minuteVal -= 60;
      while (minuteVal < 0)  minuteVal += 60;
      break;

    case 2:
      if (pm) hour24 -= 12;
      else    hour24 += 12;
      break;
  }

  displayDirty = true;
}

void applyDateFieldDelta(int idx, int delta) {
  if (delta == 0) return;

  switch (idx) {
    case 3:
      currentDay += delta;
      while (currentDay > Time::kSaturday) currentDay -= 7;
      while (currentDay < Time::kSunday)   currentDay += 7;
      break;

    case 4:
      currentMonth += delta;
      while (currentMonth > 12) currentMonth -= 12;
      while (currentMonth < 1)  currentMonth += 12;
      clampCurrentDateToMonth();
      break;

    case 5: {
      currentDate += delta;
      int dim = daysInMonth(currentYear, currentMonth);
      while (currentDate > dim) currentDate -= dim;
      while (currentDate < 1)   currentDate += dim;
      break;
    }
  }

  displayDirty = true;
}

void renderTimeLine(int hour24, int minuteVal, bool editModeActive) {
  char line1[17];
  fillLine(line1);

  int displayHour = getDisplayHour12From(hour24);
  bool pm = isPMFrom(hour24);

  char hourField[3];
  char minuteField[3];
  char periodField[3];

  snprintf(hourField, sizeof(hourField), "%02d", displayHour);
  snprintf(minuteField, sizeof(minuteField), "%02d", minuteVal);
  snprintf(periodField, sizeof(periodField), "%s", pm ? "PM" : "AM");

  if (editModeActive && !blinkVisible) {
    if (editIndex == 0) {
      hourField[0] = ' ';
      hourField[1] = ' ';
    } else if (editIndex == 1) {
      minuteField[0] = ' ';
      minuteField[1] = ' ';
    } else if (editIndex == 2) {
      periodField[0] = ' ';
      periodField[1] = ' ';
    }
  }

  line1[4]  = hourField[0];
  line1[5]  = hourField[1];
  line1[6]  = ':';
  line1[7]  = minuteField[0];
  line1[8]  = minuteField[1];
  line1[9]  = ' ';
  line1[10] = periodField[0];
  line1[11] = periodField[1];

  updateLineIfChanged(0, line1, lastLine1);
}

void renderDateLine(bool editModeActive) {
  char line2[17];
  fillLine(line2);

  char dayField[4];
  char monthField[4];
  char dateField[3];

  snprintf(dayField, sizeof(dayField), "%s", dayShort(currentDay));
  snprintf(monthField, sizeof(monthField), "%s", monthShort(currentMonth));
  snprintf(dateField, sizeof(dateField), "%02d", currentDate);

  if (editModeActive && !blinkVisible) {
    if (editIndex == 3) {
      dayField[0] = ' ';
      dayField[1] = ' ';
      dayField[2] = ' ';
    } else if (editIndex == 4) {
      monthField[0] = ' ';
      monthField[1] = ' ';
      monthField[2] = ' ';
    } else if (editIndex == 5) {
      dateField[0] = ' ';
      dateField[1] = ' ';
    }
  }

  line2[0]  = dayField[0];
  line2[1]  = dayField[1];
  line2[2]  = dayField[2];
  line2[9]  = monthField[0];
  line2[10] = monthField[1];
  line2[11] = monthField[2];
  line2[13] = dateField[0];
  line2[14] = dateField[1];

  updateLineIfChanged(1, line2, lastLine2);
}

void renderAlarmDaysLine() {
  char line2[17];
  fillLine(line2);

  const uint8_t pos[7] = {1, 3, 5, 7, 9, 11, 13};

  for (int i = 0; i < 7; i++) {
    if (editIndex == (3 + i) && !blinkVisible) {
      line2[pos[i]] = ' ';
    } else if (alarmDayEnabled(i)) {
      line2[pos[i]] = '~';
    } else {
      line2[pos[i]] = normalDayLetter(i);
    }
  }

  updateLineIfChanged(1, line2, lastLine2);

  for (int i = 0; i < 7; i++) {
    if (editIndex == (3 + i) && !blinkVisible) continue;

    if (alarmDayEnabled(i)) {
      lcd.setCursor(pos[i], 1);
      lcd.write_char(underlinedCharForDay(i));
      lastLine2[pos[i]] = '~';
    }
  }
}

void renderNormalMode(bool fullRedraw) {
  if (fullRedraw) clearLineCaches();
  renderTimeLine(currentHour, currentMinute, false);
  renderDateLine(false);
  lcd.noCursor();
  lcd.stopBlink();
}

void renderSetTimeMode(bool fullRedraw) {
  if (fullRedraw) clearLineCaches();
  renderTimeLine(currentHour, currentMinute, true);
  renderDateLine(true);
  lcd.noCursor();
  lcd.stopBlink();
}

void renderSetAlarmMode(bool fullRedraw) {
  if (fullRedraw) clearLineCaches();
  renderTimeLine(alarmHour, alarmMinute, true);
  renderAlarmDaysLine();
  lcd.noCursor();
  lcd.stopBlink();
}

void updateDisplayIfNeeded() {
  unsigned long now = millis();
  if (!displayDirty) return;
  if (now - lastLcdUpdate < lcdUpdatePeriod) return;

  lastLcdUpdate = now;
  bool fullRedraw = (mode != lastRenderedMode);

  if (mode == MODE_NORMAL) {
    renderNormalMode(fullRedraw);
  } else if (mode == MODE_SET_TIME) {
    renderSetTimeMode(fullRedraw);
  } else {
    renderSetAlarmMode(fullRedraw);
  }

  lastRenderedMode = mode;
  displayDirty = false;
}

// ---------------- RTC helpers ----------------
void syncCurrentDateTimeFromRTC() {
  Time t = rtc.time();

  if (t.hr >= 0 && t.hr < 24 &&
      t.min >= 0 && t.min < 60 &&
      t.mon >= 1 && t.mon <= 12 &&
      t.date >= 1 && t.date <= 31 &&
      t.day >= Time::kSunday && t.day <= Time::kSaturday) {

    bool changed = false;

    if (t.hr != currentHour)    { currentHour = t.hr; changed = true; }
    if (t.min != currentMinute) { currentMinute = t.min; changed = true; }
    if (t.day != currentDay)    { currentDay = t.day; changed = true; }
    if (t.mon != currentMonth)  { currentMonth = t.mon; changed = true; }
    if (t.date != currentDate)  { currentDate = t.date; changed = true; }
    if (t.yr != currentYear)    { currentYear = t.yr; changed = true; }

    if (changed) displayDirty = true;
  }
}

void writeEditedDateTimeToRTC() {
  clampCurrentDateToMonth();
  Time t = rtc.time();

  Time newTime(
    currentYear,
    currentMonth,
    currentDate,
    currentHour,
    currentMinute,
    t.sec,
    (Time::Day)currentDay
  );

  rtc.time(newTime);
}

// ---------------- Mode transitions ----------------
void enterTimeMode() {
  syncCurrentDateTimeFromRTC();
  mode = MODE_SET_TIME;
  editIndex = 0;
  blinkVisible = true;
  lastBlinkToggle = millis();
  setBacklightForMode();
  displayDirty = true;
}

void enterAlarmMode() {
  writeEditedDateTimeToRTC();
  successMini();
  mode = MODE_SET_ALARM;
  editIndex = 0;
  blinkVisible = true;
  lastBlinkToggle = millis();
  setBacklightForMode();
  displayDirty = true;
}

void exitToNormalMode() {
  saveAlarmToEEPROM();
  successMini();
  mode = MODE_NORMAL;
  blinkVisible = true;
  setBacklightForMode();
  displayDirty = true;
}

// ---------------- Encoder ISR ----------------
void updateEncoderISR() {
  uint8_t portd = PIND;
  uint8_t a = (portd >> 2) & 0x01;
  uint8_t b = (portd >> 3) & 0x01;
  uint8_t newState = (a << 1) | b;
  uint8_t index = (encoderState << 2) | newState;

  encoderAcc += transitionTable[index];
  encoderState = newState;
}

void isrA() {
  updateEncoderISR();
}

void isrB() {
  updateEncoderISR();
}

// ---------------- Setup ----------------
void setup() {
  pinMode(pinA, INPUT_PULLUP);
  pinMode(pinB, INPUT_PULLUP);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(rotButtonPin, INPUT_PULLUP);
  pinMode(buzzer, OUTPUT);
  pinMode(emitterPin, OUTPUT);
  pinMode(motorPin, OUTPUT);
  pinMode(sensorPin, INPUT);

  digitalWrite(emitterPin, LOW);

  Serial.begin(115200);

  uint8_t a = digitalRead(pinA);
  uint8_t b = digitalRead(pinB);
  encoderState = (a << 1) | b;

  attachInterrupt(digitalPinToInterrupt(pinA), isrA, CHANGE);
  attachInterrupt(digitalPinToInterrupt(pinB), isrB, CHANGE);

  loadAlarmFromEEPROM();

  rtc.writeProtect(false);
  rtc.halt(false);

  // Uncomment once, upload, then comment again and re-upload.
  // Time t(2026, 3, 15, 21, 13, 50, Time::kSunday);
  // rtc.time(t);

  syncCurrentDateTimeFromRTC();

  lcd.init();
  lcd.customSymbol(0, UL_M);
  lcd.customSymbol(1, UL_T);
  lcd.customSymbol(2, UL_W);
  lcd.customSymbol(3, UL_F);
  lcd.customSymbol(4, UL_S);

  lcd.setRGB(255, 255, 255);
  lcd.clear();

  setBacklightForMode();
  displayDirty = true;
  updateDisplayIfNeeded();

  calibratePhotodiodeBaseline();
}

// ---------------- Loop ----------------
void loop() {

  // ---------------- Alarm trigger ----------------
  if (!alarmRinging && mode == MODE_NORMAL) {
    int dayIdx = (currentDay == Time::kSunday) ? 6 : (currentDay - 1);

    if (!alarmLatched &&
        alarmDayEnabled(dayIdx) &&
        currentHour == alarmHour &&
        currentMinute == alarmMinute) {
      startCasioAlarm();
      alarmLatched = true;
    }

    // reset latch when minute changes
    if (currentMinute != alarmMinute) {
      alarmLatched = false;
    }
  }

  if (alarmRinging) {
    serviceCasioAlarm();

    bool buttonPressed =
      fallingEdgeDebounced(buttonPin, lastButtonState, lastButtonTime) ||
      fallingEdgeDebounced(rotButtonPin, lastRotButtonState, lastRotButtonTime);

    bool proximityTriggered = readProximityDetected();

    

    if (proximityTriggered){
      digitalWrite(motorPin,HIGH);
    }

    if (buttonPressed) {
      stopCasioAlarm();
      digitalWrite(motorPin,LOW);

      return;
    }


  } else {
    digitalWrite(emitterPin, LOW);
    
  }

  if (mode == MODE_NORMAL) {
    unsigned long now = millis();
    if (now - lastRtcPoll >= rtcPollPeriodMs) {
      lastRtcPoll = now;
      syncCurrentDateTimeFromRTC();
    }
  }

  if (mode != MODE_NORMAL) {
    unsigned long now = millis();
    if (now - lastBlinkToggle >= blinkPeriod) {
      lastBlinkToggle = now;
      blinkVisible = !blinkVisible;
      displayDirty = true;
    }
  }

  int8_t acc;
  noInterrupts();
  acc = encoderAcc;
  encoderAcc = 0;
  interrupts();

  static int subStepAccumulator = 0;
  subStepAccumulator += acc;

  int step = 0;
  while (subStepAccumulator >= 4) {
    step++;
    subStepAccumulator -= 4;
  }
  while (subStepAccumulator <= -4) {
    step--;
    subStepAccumulator += 4;
  }

  if (step != 0) {
    if (step > 0) rotaryCW();
    else rotaryCCW();

    blinkVisible = true;
    lastBlinkToggle = millis();

    if (mode == MODE_SET_TIME) {
      if (editIndex <= 2) {
        applyFieldDeltaTo(editIndex, step, currentHour, currentMinute);
      } else {
        applyDateFieldDelta(editIndex, step);
      }
    } else if (mode == MODE_SET_ALARM) {
      if (editIndex <= 2) {
        applyFieldDeltaTo(editIndex, step, alarmHour, alarmMinute);
      } else {
        int dayIdx = editIndex - 3;
        setAlarmDayEnabled(dayIdx, !alarmDayEnabled(dayIdx));
      }
    }
  }

  if (mode == MODE_NORMAL) {
    if (fallingEdgeDebounced(buttonPin, lastButtonState, lastButtonTime)) {
      glassClick();
      enterTimeMode();
      return;
    }
  } else if (mode == MODE_SET_TIME) {
    if (fallingEdgeDebounced(rotButtonPin, lastRotButtonState, lastRotButtonTime)) {
      glassClick();
      editIndex = (editIndex + 1) % 6;
      blinkVisible = true;
      lastBlinkToggle = millis();
      displayDirty = true;
    }

    if (fallingEdgeDebounced(buttonPin, lastButtonState, lastButtonTime)) {
      glassClick();
      enterAlarmMode();
      return;
    }
  } else if (mode == MODE_SET_ALARM) {
    if (fallingEdgeDebounced(rotButtonPin, lastRotButtonState, lastRotButtonTime)) {
      glassClick();
      editIndex = (editIndex + 1) % 10;
      blinkVisible = true;
      lastBlinkToggle = millis();
      displayDirty = true;
    }

    if (fallingEdgeDebounced(buttonPin, lastButtonState, lastButtonTime)) {
      glassClick();
      exitToNormalMode();
      return;
    }
  }

  updateDisplayIfNeeded();
}
