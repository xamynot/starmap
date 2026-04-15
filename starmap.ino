#include <Wire.h>
#include <Adafruit_MCP4728.h>
#include <math.h>
#include <EEPROM.h>

Adafruit_MCP4728 dac;

#define DAC_CH_A MCP4728_CHANNEL_A
#define DAC_CH_C MCP4728_CHANNEL_C
#define KNOB_PIN_1 A0          // GP26
#define KNOB_PIN_2 A1          // GP27
#define INPUT_PIN_1 2
#define INPUT_PIN_2 3
#define GATE_PIN 10
#define VREF 5.0

// ===== 按钮引脚定义 =====
#define BTN_LEFT 6
#define BTN_RIGHT 7
#define BTN_CONFIRM 8

// ===== LED 引脚定义（12个半音）=====
const int ledPins[] = {
  12,  // C
  13,  // C# / Db
  14,  // D
  15,  // D# / Eb
  16,  // E
  17,  // F
  18,  // F# / Gb
  19,  // G
  20,  // G# / Ab
  21,  // A
  22,  // A# / Bb
  0    // B
};
#define LED_COUNT 12

// EEPROM 地址定义
#define EEPROM_ADDR_SCALE_FACTOR_1 0
#define EEPROM_ADDR_SCALE_FACTOR_2 4
#define EEPROM_MAGIC_NUMBER_1 0xA5
#define EEPROM_MAGIC_NUMBER_2 0xA6
#define EEPROM_ADDR_MAGIC_1 100
#define EEPROM_ADDR_MAGIC_2 101
#define EEPROM_ADDR_NOTE_ENABLE 200

enum Mode {
  MODE_PLAY_AUTO,
  MODE_KNOB,
  MODE_CALIBRATE_1,
  MODE_CALIBRATE_2,
  MODE_EDIT_SCALE
};

Mode currentMode = MODE_KNOB;

float scaleFactor1 = 1.0;
float scaleFactor2 = 1.0;

// 基础音阶表（12个半音）
float baseScaleTable[] = {
  0.0/12.0,      // C
  1.0/12.0,      // C#
  2.0/12.0,      // D
  3.0/12.0,      // D#
  4.0/12.0,      // E
  5.0/12.0,      // F
  6.0/12.0,      // F#
  7.0/12.0,      // G
  8.0/12.0,      // G#
  9.0/12.0,      // A
  10.0/12.0,     // A#
  11.0/12.0,     // B
  12.0/12.0      // C (八度)
};

// 实际使用的音阶
float scaleTable[13];
int scaleSize = 0;

// 工作副本（编辑时修改）
bool noteEnabledWorking[12];
bool noteEnabled[12];

#define MAX_OCTAVE 4

int stepIndex = 0;
int direction = 1;
int totalSteps = 0;

unsigned long lastPrintTime = 0;
const unsigned long PRINT_INTERVAL = 100;

float knobSmooth1 = 0.0;
float knobSmooth2 = 0.0;

// ===== 编辑模式变量 =====
int selectedNote = 0;
bool ledBlinkState = false;
unsigned long lastBlinkTime = 0;
const unsigned long BLINK_INTERVAL = 300;
bool editModeActive = false;

// ===== 长按检测变量 =====
unsigned long confirmPressStartTime = 0;
bool confirmPressedFlag = false;

// ===== 按钮去抖动 =====
unsigned long lastButtonTime = 0;
const unsigned long DEBOUNCE_DELAY = 200;
const unsigned long LONG_PRESS_TIME = 2000;  // 长按 2 秒

// ===== LED 闪烁提示 =====
bool flashingActive = false;
unsigned long flashStartTime = 0;
const unsigned long FLASH_INTERVAL = 150;
const int FLASH_TIMES = 2;

// ===== 检查是否有启用的音符 =====
bool hasEnabledNotes() {
  for (int i = 0; i < 12; i++) {
    if (noteEnabled[i]) return true;
  }
  return false;
}

// ===== 更新音阶表 =====
void updateScaleTable() {
  scaleSize = 0;
  for (int i = 0; i < 12; i++) {
    if (noteEnabled[i]) {
      scaleTable[scaleSize] = baseScaleTable[i];
      scaleSize++;
    }
  }
  if (noteEnabled[0]) {
    scaleTable[scaleSize] = baseScaleTable[12];
    scaleSize++;
  }
  
  totalSteps = scaleSize * MAX_OCTAVE;
  
  if (scaleSize == 0) {
    Serial.println("Scale is empty (all notes disabled) - using continuous voltage mode");
  } else {
    Serial.print("Scale updated: ");
    for (int i = 0; i < scaleSize; i++) {
      Serial.print(scaleTable[i] * 12, 0);
      Serial.print(" ");
    }
    Serial.println();
  }
}

// ===== 保存音符启用状态到 Flash =====
void saveNoteEnabledToFlash() {
  EEPROM.begin(512);
  for (int i = 0; i < 12; i++) {
    EEPROM.write(EEPROM_ADDR_NOTE_ENABLE + i, noteEnabled[i] ? 1 : 0);
  }
  EEPROM.commit();
  EEPROM.end();
  Serial.println("✓ Saved note enable states to Flash");
}

// ===== LED 闪烁反馈 =====
void startSaveFeedback() {
  flashingActive = true;
  flashStartTime = millis();
  // 立即关闭所有 LED
  for (int i = 0; i < LED_COUNT; i++) {
    digitalWrite(ledPins[i], LOW);
  }
}

void updateFlashFeedback() {
  if (!flashingActive) return;
  
  unsigned long now = millis();
  int currentFlashIndex = (now - flashStartTime) / FLASH_INTERVAL;
  
  if (currentFlashIndex >= FLASH_TIMES * 2) {
    // 闪烁结束
    flashingActive = false;
    showScaleLEDs();
    return;
  }
  
  // 奇数次闪烁：点亮，偶数次：熄灭
  bool shouldBeOn = (currentFlashIndex % 2 == 0);
  
  for (int i = 0; i < LED_COUNT; i++) {
    digitalWrite(ledPins[i], shouldBeOn ? HIGH : LOW);
  }
}

// ===== 从 Flash 加载 =====
void loadNoteEnabledFromFlash() {
  EEPROM.begin(512);
  bool hasData = false;
  for (int i = 0; i < 12; i++) {
    int val = EEPROM.read(EEPROM_ADDR_NOTE_ENABLE + i);
    if (val == 0 || val == 1) {
      noteEnabled[i] = (val == 1);
      hasData = true;
    }
  }
  EEPROM.end();
  
  if (!hasData) {
    for (int i = 0; i < 12; i++) noteEnabled[i] = true;
    Serial.println("No saved note states, using default (all enabled)");
  } else {
    Serial.println("Loaded note enable states from Flash");
  }
  
  for (int i = 0; i < 12; i++) noteEnabledWorking[i] = noteEnabled[i];
  updateScaleTable();
}

// ===== LED 控制 =====
void initLEDs() {
  for (int i = 0; i < LED_COUNT; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }
}

void showScaleLEDs() {
  for (int i = 0; i < LED_COUNT; i++) {
    digitalWrite(ledPins[i], noteEnabled[i] ? HIGH : LOW);
  }
}

void updateEditLEDs() {
  for (int i = 0; i < LED_COUNT; i++) {
    if (i == selectedNote) continue;
    digitalWrite(ledPins[i], noteEnabledWorking[i] ? HIGH : LOW);
  }
  
  if (ledBlinkState) {
    digitalWrite(ledPins[selectedNote], HIGH);
  } else {
    digitalWrite(ledPins[selectedNote], LOW);
  }
}

void printNoteName(int note) {
  const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  Serial.print(noteNames[note]);
}

// ===== 应用更改并退出（不保存）=====
void applyAndExitEditMode() {
  if (!editModeActive) return;
  
  // 应用更改到实际音阶
  for (int i = 0; i < 12; i++) {
    noteEnabled[i] = noteEnabledWorking[i];
  }
  updateScaleTable();
  
  editModeActive = false;
  showScaleLEDs();
  Serial.println("✓ Changes applied (not saved to Flash)");
}

// ===== 保存并退出编辑模式（长按）=====
void saveAndExitEditMode() {
  if (!editModeActive) return;
  
  // 应用更改到实际音阶
  for (int i = 0; i < 12; i++) {
    noteEnabled[i] = noteEnabledWorking[i];
  }
  updateScaleTable();
  saveNoteEnabledToFlash();
  startSaveFeedback();
  
  editModeActive = false;
  if (!flashingActive) {
    showScaleLEDs();
  }
  Serial.println("✓ Changes SAVED to Flash");
}

// ===== 进入编辑模式 =====
void enterEditMode() {
  editModeActive = true;
  for (int i = 0; i < 12; i++) {
    noteEnabledWorking[i] = noteEnabled[i];
  }
  selectedNote = 0;
  ledBlinkState = true;
  lastBlinkTime = millis();
  confirmPressStartTime = 0;
  confirmPressedFlag = false;
  Serial.println("=== EDIT SCALE MODE ===");
  Serial.println("LEFT/RIGHT: select note | SHORT CONFIRM: toggle + exit | LONG CONFIRM (2s): save + exit");
  Serial.print("Selected: ");
  printNoteName(selectedNote);
  Serial.println();
  updateEditLEDs();
}

// ===== 按钮处理 =====
void handleButtons() {
  unsigned long now = millis();
  
  bool leftPressed = digitalRead(BTN_LEFT) == LOW;
  bool rightPressed = digitalRead(BTN_RIGHT) == LOW;
  bool confirmPressed = digitalRead(BTN_CONFIRM) == LOW;
  
  // 非编辑模式：按任意按钮进入编辑
  if (!editModeActive && !flashingActive && (leftPressed || rightPressed || confirmPressed)) {
    if (now - lastButtonTime >= DEBOUNCE_DELAY) {
      enterEditMode();
      lastButtonTime = now;
    }
    return;
  }
  
  // 编辑模式
  if (editModeActive && !flashingActive) {
    // 左键：选择上一个音符
    if (leftPressed && (now - lastButtonTime >= DEBOUNCE_DELAY)) {
      selectedNote--;
      if (selectedNote < 0) selectedNote = 11;
      Serial.print("Selected: ");
      printNoteName(selectedNote);
      Serial.println();
      updateEditLEDs();
      lastButtonTime = now;
      confirmPressStartTime = 0;
      confirmPressedFlag = false;
    }
    // 右键：选择下一个音符
    else if (rightPressed && (now - lastButtonTime >= DEBOUNCE_DELAY)) {
      selectedNote++;
      if (selectedNote >= 12) selectedNote = 0;
      Serial.print("Selected: ");
      printNoteName(selectedNote);
      Serial.println();
      updateEditLEDs();
      lastButtonTime = now;
      confirmPressStartTime = 0;
      confirmPressedFlag = false;
    }
    // 确认键（支持长按/短按）
    else if (confirmPressed) {
      if (!confirmPressedFlag) {
        confirmPressStartTime = now;
        confirmPressedFlag = true;
      } else if ((now - confirmPressStartTime) >= LONG_PRESS_TIME) {
        // 长按触发保存并退出
        confirmPressedFlag = false;
        confirmPressStartTime = 0;
        saveAndExitEditMode();
        lastButtonTime = now;
      }
    } else {
      // 确认键释放
      if (confirmPressedFlag && (now - confirmPressStartTime) > 0 && (now - confirmPressStartTime) < LONG_PRESS_TIME) {
        // 短按：翻转当前音符状态，然后应用并退出
        noteEnabledWorking[selectedNote] = !noteEnabledWorking[selectedNote];
        Serial.print("Toggle ");
        printNoteName(selectedNote);
        Serial.print(": ");
        Serial.println(noteEnabledWorking[selectedNote] ? "ENABLED" : "DISABLED");
        applyAndExitEditMode();
        lastButtonTime = now;
      }
      confirmPressStartTime = 0;
      confirmPressedFlag = false;
    }
  }
}

// ===== 频率测量 =====
volatile unsigned long lastTime1 = 0;
volatile unsigned long lastTime2 = 0;
volatile unsigned long period1 = 0;
volatile unsigned long period2 = 0;
volatile bool newPeriod1 = false;
volatile bool newPeriod2 = false;

void onRising1() {
  unsigned long now = micros();
  unsigned long dt = now - lastTime1;
  if (dt > 0 && dt < 100000) {
    period1 = dt;
    newPeriod1 = true;
  }
  lastTime1 = now;
}

void onRising2() {
  unsigned long now = micros();
  unsigned long dt = now - lastTime2;
  if (dt > 0 && dt < 100000) {
    period2 = dt;
    newPeriod2 = true;
  }
  lastTime2 = now;
}

float getFrequency(int vcoNum, int samples = 20) {
  float sum = 0;
  int count = 0;
  
  for (int i = 0; i < samples; i++) {
    unsigned long p = 0;
    bool hasNew = false;
    
    if (vcoNum == 1) {
      if (newPeriod1) {
        p = period1;
        newPeriod1 = false;
        hasNew = true;
      }
    } else {
      if (newPeriod2) {
        p = period2;
        newPeriod2 = false;
        hasNew = true;
      }
    }
    
    if (hasNew && p > 0) {
      sum += 1000000.0 / p;
      count++;
    }
    delay(5);
  }
  
  if (count == 0) return 0;
  return sum / count;
}

// ===== Flash 存储（V/Oct 校准）=====
void saveScaleFactorsToFlash() {
  EEPROM.begin(512);
  
  byte* p1 = (byte*)(void*)&scaleFactor1;
  for (int i = 0; i < sizeof(float); i++) {
    EEPROM.write(EEPROM_ADDR_SCALE_FACTOR_1 + i, *p1++);
  }
  EEPROM.write(EEPROM_ADDR_MAGIC_1, EEPROM_MAGIC_NUMBER_1);
  
  byte* p2 = (byte*)(void*)&scaleFactor2;
  for (int i = 0; i < sizeof(float); i++) {
    EEPROM.write(EEPROM_ADDR_SCALE_FACTOR_2 + i, *p2++);
  }
  EEPROM.write(EEPROM_ADDR_MAGIC_2, EEPROM_MAGIC_NUMBER_2);
  
  EEPROM.commit();
  EEPROM.end();
  
  Serial.println("✓ Saved calibration for both VCOs");
}

bool loadScaleFactorsFromFlash() {
  EEPROM.begin(512);
  bool valid1 = false, valid2 = false;
  
  if (EEPROM.read(EEPROM_ADDR_MAGIC_1) == EEPROM_MAGIC_NUMBER_1) {
    float loaded = 0;
    byte* p = (byte*)(void*)&loaded;
    for (int i = 0; i < sizeof(float); i++) {
      *p++ = EEPROM.read(EEPROM_ADDR_SCALE_FACTOR_1 + i);
    }
    if (loaded >= 0.5 && loaded <= 2.0) {
      scaleFactor1 = loaded;
      valid1 = true;
    }
  }
  
  if (EEPROM.read(EEPROM_ADDR_MAGIC_2) == EEPROM_MAGIC_NUMBER_2) {
    float loaded = 0;
    byte* p = (byte*)(void*)&loaded;
    for (int i = 0; i < sizeof(float); i++) {
      *p++ = EEPROM.read(EEPROM_ADDR_SCALE_FACTOR_2 + i);
    }
    if (loaded >= 0.5 && loaded <= 2.0) {
      scaleFactor2 = loaded;
      valid2 = true;
    }
  }
  
  EEPROM.end();
  
  Serial.print("VCO1 scaleFactor: ");
  Serial.println(scaleFactor1, 4);
  Serial.print("VCO2 scaleFactor: ");
  Serial.println(scaleFactor2, 4);
  
  return valid1 && valid2;
}

float readKnob(int pin, float &smooth) {
  int raw = analogRead(pin);
  float v = raw / 4095.0;
  smooth = smooth * 0.85 + v * 0.15;
  return smooth;
}

uint16_t voltageToDAC(float v) {
  if (v < 0) v = 0;
  if (v > VREF) v = VREF;
  return (uint16_t)((v / VREF) * 4095.0);
}

void setVoltageA(float v) {
  dac.setChannelValue(DAC_CH_A, voltageToDAC(v), MCP4728_VREF_VDD, MCP4728_GAIN_1X);
}

void setVoltageC(float v) {
  dac.setChannelValue(DAC_CH_C, voltageToDAC(v), MCP4728_VREF_VDD, MCP4728_GAIN_1X);
}

// ===== 校准函数 =====
void calibrateVCO(int vcoNum, float &scaleFactor, int dacChannel) {
  Serial.print("=== Calibrating VCO ");
  Serial.print(vcoNum);
  Serial.println(" ===");
  Serial.println("Output: 0V, 1V, 2V, 3V, 4V");
  Serial.println("Make sure VCO is connected and making sound!");
  Serial.println();

  digitalWrite(GATE_PIN, LOW);

  float frequencies[5];
  bool valid[5] = {false, false, false, false, false};

  for (int v = 0; v <= 4; v++) {
    Serial.print("Output ");
    Serial.print(v);
    Serial.print("V...");
    
    if (dacChannel == 0) setVoltageA(v);
    else setVoltageC(v);
    
    delay(800);
    
    float freq = getFrequency(vcoNum, 30);
    
    if (freq > 0) {
      frequencies[v] = freq;
      valid[v] = true;
      Serial.print(" -> ");
      Serial.print(freq, 2);
      Serial.println(" Hz");
    } else {
      Serial.println(" -> NO SIGNAL!");
    }
  }

  if (!valid[0]) {
    Serial.println("ERROR: No signal detected at 0V! Check VCO connection.");
    return;
  }

  float totalRatio = 0;
  int validCount = 0;

  Serial.println();
  Serial.print("=== VCO ");
  Serial.print(vcoNum);
  Serial.println(" Results ===");

  for (int v = 1; v <= 4; v++) {
    if (valid[v] && valid[0]) {
      float actualSemitones = log2(frequencies[v] / frequencies[0]) * 12;
      float expectedSemitones = v * 12;
      float ratio = expectedSemitones / actualSemitones;
      
      totalRatio += ratio;
      validCount++;
      
      Serial.print("  ");
      Serial.print(v);
      Serial.print("V: ");
      Serial.print(frequencies[v], 2);
      Serial.print(" Hz (expected ");
      Serial.print(frequencies[0] * pow(2, v), 2);
      Serial.print(" Hz) ratio=");
      Serial.println(ratio, 4);
    }
  }

  if (validCount > 0) {
    scaleFactor = totalRatio / validCount;
    Serial.print(">>> New scaleFactor: ");
    Serial.println(scaleFactor, 4);
  } else {
    Serial.println("Calibration failed!");
  }

  Serial.println();
}

void runCalibration1() {
  calibrateVCO(1, scaleFactor1, 0);
  saveScaleFactorsToFlash();
  currentMode = MODE_KNOB;
  showScaleLEDs();
}

void runCalibration2() {
  calibrateVCO(2, scaleFactor2, 1);
  saveScaleFactorsToFlash();
  currentMode = MODE_KNOB;
  showScaleLEDs();
}

// ===== 量化电压计算（如果有音阶）或连续电压 =====
float knobToVoltage(float knob, float scaleFactor) {
  float inputVoltage = knob * 20.0;
  if (inputVoltage > 5.0) inputVoltage = 5.0;
  if (inputVoltage < 0) inputVoltage = 0;
  
  // 如果没有启用的音符，直接输出连续电压
  if (scaleSize == 0) {
    return inputVoltage * scaleFactor;
  }
  
  int octave = (int)inputVoltage;
  if (octave > MAX_OCTAVE) octave = MAX_OCTAVE;
  
  float positionInOctave = inputVoltage - octave;
  
  float step = 1.0 / scaleSize;
  int idx = (int)(positionInOctave / step);
  if (idx >= scaleSize) idx = scaleSize - 1;
  
  return (scaleTable[idx] + octave) * scaleFactor;
}

float calculateAutoVoltage() {
  // 如果没有启用的音符，不输出
  if (scaleSize == 0) return 0;
  
  int octave = stepIndex / scaleSize;
  int noteIndex = stepIndex % scaleSize;
  float v = scaleTable[noteIndex] + octave;
  
  stepIndex += direction;
  if (stepIndex >= totalSteps) {
    stepIndex = totalSteps - 2;
    direction = -1;
  }
  if (stepIndex < 0) {
    stepIndex = 1;
    direction = 1;
  }
  return v;
}

void handleSerial() {
  if (Serial.available()) {
    char cmd = Serial.read();
    
    if (cmd == 'a') {
      currentMode = MODE_PLAY_AUTO;
      stepIndex = 0;
      direction = 1;
      Serial.println("AUTO mode");
    }
    else if (cmd == 'k') {
      currentMode = MODE_KNOB;
      Serial.println("KNOB mode");
    }
    else if (cmd == '1') {
      currentMode = MODE_CALIBRATE_1;
      Serial.println("Starting VCO1 calibration...");
    }
    else if (cmd == '2') {
      currentMode = MODE_CALIBRATE_2;
      Serial.println("Starting VCO2 calibration...");
    }
    else if (cmd == 's') {
      saveScaleFactorsToFlash();
    }
    else if (cmd == 'r') {
      scaleFactor1 = 1.0;
      scaleFactor2 = 1.0;
      saveScaleFactorsToFlash();
      Serial.println("Reset both to 1.0");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Wire.begin();
  dac.begin();
  
  pinMode(KNOB_PIN_1, INPUT);
  pinMode(KNOB_PIN_2, INPUT);
  pinMode(INPUT_PIN_1, INPUT_PULLUP);
  pinMode(INPUT_PIN_2, INPUT_PULLUP);
  pinMode(GATE_PIN, OUTPUT);
  
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_CONFIRM, INPUT_PULLUP);
  
  digitalWrite(GATE_PIN, LOW);
  
  initLEDs();
  
  attachInterrupt(digitalPinToInterrupt(INPUT_PIN_1), onRising1, RISING);
  attachInterrupt(digitalPinToInterrupt(INPUT_PIN_2), onRising2, RISING);
  
  loadScaleFactorsFromFlash();
  loadNoteEnabledFromFlash();
  
  setVoltageA(0);
  setVoltageC(0);
  
  showScaleLEDs();
  
  Serial.println("=== DUAL VCO SYSTEM with Scale Editor ===");
  Serial.println("Press any button to edit scale");
  Serial.println("In edit mode:");
  Serial.println("  LEFT/RIGHT - select note");
  Serial.println("  SHORT CONFIRM - toggle note + exit (NO save)");
  Serial.println("  LONG CONFIRM (2s) - save to Flash + LED flash + exit");
  Serial.println("When all notes disabled: continuous voltage mode");
  Serial.println("Commands: 'a'=AUTO, 'k'=KNOB, '1'=Cal VCO1, '2'=Cal VCO2");
}

void loop() {
  handleSerial();
  
  // 处理 LED 闪烁反馈（优先级最高）
  if (flashingActive) {
    updateFlashFeedback();
  }
  
  // 处理按钮
  if (currentMode != MODE_CALIBRATE_1 && currentMode != MODE_CALIBRATE_2 && !flashingActive) {
    handleButtons();
  }
  
  // 编辑模式 LED 闪烁
  if (editModeActive && !flashingActive) {
    unsigned long now = millis();
    if (now - lastBlinkTime >= BLINK_INTERVAL) {
      lastBlinkTime = now;
      ledBlinkState = !ledBlinkState;
      updateEditLEDs();
    }
  }
  
  // 校准模式
  if (currentMode == MODE_CALIBRATE_1) {
    runCalibration1();
    return;
  }
  if (currentMode == MODE_CALIBRATE_2) {
    runCalibration2();
    return;
  }
  
  // ===== 音频输出 =====
  float v1, v2;
  
  if (currentMode == MODE_KNOB) {
    float knob1 = readKnob(KNOB_PIN_1, knobSmooth1);
    float knob2 = readKnob(KNOB_PIN_2, knobSmooth2);
    
    v1 = knobToVoltage(knob1, scaleFactor1);
    v2 = knobToVoltage(knob2, scaleFactor2);
    
    setVoltageA(v1);
    setVoltageC(v2);
    
    unsigned long now = millis();
    if (now - lastPrintTime >= PRINT_INTERVAL) {
      lastPrintTime = now;
      Serial.print("K1=");
      Serial.print(knob1, 3);
      Serial.print(" VCO1=");
      Serial.print(v1, 3);
      Serial.print(" | K2=");
      Serial.print(knob2, 3);
      Serial.print(" VCO2=");
      Serial.println(v2, 3);
    }
  } else if (currentMode == MODE_PLAY_AUTO) {
    float autoV = calculateAutoVoltage();
    v1 = autoV * scaleFactor1;
    v2 = autoV * scaleFactor2;
    
    setVoltageA(v1);
    setVoltageC(v2);
    
    unsigned long now = millis();
    if (now - lastPrintTime >= PRINT_INTERVAL) {
      lastPrintTime = now;
      Serial.print("Auto: VCO1=");
      Serial.print(v1, 3);
      Serial.print(" VCO2=");
      Serial.println(v2, 3);
    }
    delay(150);
  }
}