#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_MAX31865.h>
#include <Encoder.h>
#include <TimerOne.h>

// ===== ПИНЫ ДИСПЛЕЯ =====
#define TFT_CS    10
#define TFT_RST   8
#define TFT_DC    9
#define TFT_BL    5

// ===== ПИНЫ ЭНКОДЕРА =====
#define ENC_CLK   2
#define ENC_DT    3
#define ENC_SW    4

// ===== ПИНЫ ДРАЙВЕРА =====
#define DRV_EN    15   // A1 = PC1
#define DRV_STEP  16   // A2 = PC2
#define DRV_DIR   17   // A3 = PC3

// ===== ПИНЫ MAX31865 =====
#define MAX_CS    7
#define MAX_SDI   6
#define MAX_SDO   12
#define MAX_CLK   14

#define RREF      4300.0
#define RNOMINAL  1000.0

// ===== ОБЪЕКТЫ =====
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Encoder myEncoder(ENC_CLK, ENC_DT);
Adafruit_MAX31865 thermo = Adafruit_MAX31865(MAX_CS, MAX_SDI, MAX_SDO, MAX_CLK);

// ===== ПЕРЕМЕННЫЕ ЭНКОДЕРА =====
int lastPos = 0;
int lastRotationDir = 0;
int rotationDir = 0;

// ===== ПЕРЕМЕННЫЕ КНОПКИ ЭНКОДЕРА =====
bool buttonPressed = false;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;

// ===== ПЕРЕМЕННЫЕ МОТОРА =====
volatile bool motorEnabled = false;
volatile long flowRate = 0;                // мл/ч, знак = направление
volatile unsigned long stepInterval = 0;   // мкс между шагами
volatile bool stepState = false;

// ===== КАЛИБРОВКА НАСОСА =====
const float ML_PER_REV = 0.09;             // мл за один оборот (предварительно)
const float STEPS_PER_REV = 3200.0;        // 1/16 шага для 1.8° мотора
const long  MAX_FLOW = 2000;               // максимальный отбор, мл/ч

// ===== ГРАНИЦЫ ПЕРИОДА TIMER1 =====
const unsigned long MIN_INTERVAL = 10;     // мкс (оптимизированный ISR)
const unsigned long MAX_INTERVAL = 200000; // мкс (очень медленно)

// ===== ТЕМПЕРАТУРА =====
float currentTemp = 0.0;
float lastTemp = -999.0;
unsigned long lastTempUpdate = 0;

// ===== ЯРКОСТЬ =====
int brightness = 32;

// ===== ISR ТАЙМЕРА (оптимизированный, через порты) =====
void stepISR() {
  if (!motorEnabled || flowRate == 0 || stepInterval == 0) {
    PORTC &= ~(1 << PC2);   // STEP = LOW
    stepState = false;
    return;
  }
  
  if (!stepState) {
    if (flowRate > 0) PORTC |= (1 << PC3);    // DIR = HIGH
    else              PORTC &= ~(1 << PC3);   // DIR = LOW
    PORTC |= (1 << PC2);                       // STEP = HIGH
    stepState = true;
  } else {
    PORTC &= ~(1 << PC2);                      // STEP = LOW
    stepState = false;
  }
}

void setup() {
  Serial.begin(9600);
  
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  
  thermo.begin(MAX31865_4WIRE);
  
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, brightness);
  
  pinMode(ENC_SW, INPUT_PULLUP);
  
  // Настройка пинов драйвера (A1, A2, A3 — все на PORTC)
  pinMode(DRV_EN, OUTPUT);
  pinMode(DRV_STEP, OUTPUT);
  pinMode(DRV_DIR, OUTPUT);
  
  digitalWrite(DRV_EN, HIGH);   // выключен
  digitalWrite(DRV_STEP, LOW);
  digitalWrite(DRV_DIR, LOW);
  
  Timer1.initialize(1000);
  Timer1.attachInterrupt(stepISR);
  
  tft.fillScreen(ST77XX_BLACK);
  
  Serial.println("=== TFT + Энкодер + MAX31865 + DRV8825 (Timer1, port ISR) ===");
  
  drawStaticInterface();
  updateTemperatureUI();
  updateEncoderUI();
  updateButtonUI();
  updateMotorUI();
}

void loop() {
  // ----- 1. ЭНКОДЕР: УПРАВЛЕНИЕ ПОТОКОМ (мл/ч) -----
  int newPos = myEncoder.read() / 4;
  if (newPos != lastPos) {
    int delta = newPos - lastPos;
    
    long step = 1;
    if (abs(flowRate) >= 100)  step = 10;
    if (abs(flowRate) >= 500)  step = 50;
    if (abs(flowRate) >= 1000) step = 100;
    
    noInterrupts();
    flowRate += delta * step;
    flowRate = constrain(flowRate, -MAX_FLOW, MAX_FLOW);
    long spd = flowRate;
    interrupts();
    
    if (spd > 0) rotationDir = 1;
    else if (spd < 0) rotationDir = -1;
    else rotationDir = 0;
    
    if (spd == 0) {
      noInterrupts();
      stepInterval = 0;
      interrupts();
      Timer1.setPeriod(1000000);
    } else {
      float rev_per_sec   = fabs(spd) / 3600.0 / ML_PER_REV;
      float steps_per_sec = rev_per_sec * STEPS_PER_REV;
      unsigned long interval = (unsigned long)(1000000.0 / steps_per_sec);
      interval = constrain(interval, MIN_INTERVAL, MAX_INTERVAL);
      
      noInterrupts();
      stepInterval = interval;
      interrupts();
      Timer1.setPeriod(interval);
    }
    
    lastPos = newPos;
    updateEncoderUI();
    updateMotorUI();
  }

  // ----- 2. КНОПКА (неблокирующий дебаунс) -----
  bool currentButtonState = digitalRead(ENC_SW);
  if (currentButtonState != lastButtonState) {
    lastDebounceTime = millis();
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (currentButtonState == LOW && !buttonPressed) {
      buttonPressed = true;
      
      noInterrupts();
      motorEnabled = !motorEnabled;
      interrupts();
      
      if (motorEnabled) {
        digitalWrite(DRV_EN, LOW);
        Serial.println("Motor ON");
      } else {
        digitalWrite(DRV_EN, HIGH);
        PORTC &= ~(1 << PC2);   // STEP = LOW
        Serial.println("Motor OFF");
      }
      
      updateButtonUI();
      updateMotorUI();
    }
    else if (currentButtonState == HIGH && buttonPressed) {
      buttonPressed = false;
      updateButtonUI();
    }
  }
  lastButtonState = currentButtonState;

  // ----- 3. ТЕМПЕРАТУРА -----
  if (millis() - lastTempUpdate >= 500) {
    lastTempUpdate = millis();
    
    thermo.clearFault();
    float tempReading = thermo.temperature(RNOMINAL, RREF);
    
    uint8_t fault = thermo.readFault();
    if (fault) {
      currentTemp = -999.0;
      thermo.clearFault();
    } else {
      currentTemp = tempReading;
    }
    
    if (currentTemp != lastTemp) {
      updateTemperatureUI();
      lastTemp = currentTemp;
    }
  }
}

// ===== СТАТИЧЕСКИЙ ИНТЕРФЕЙС =====
void drawStaticInterface() {
  tft.drawRect(0, 0, tft.width(), tft.height(), ST77XX_WHITE);
  
  // Подпись потока
  tft.setTextColor(ST77XX_ORANGE);
  tft.setTextSize(1);
  tft.setCursor(5, 22);
  tft.print("ml/h");
  
  // Буква "C" для температуры (сдвинута вниз)
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(115, 65);
  tft.print("C");
  
  // Подпись версии
  tft.setTextColor(ST77XX_MAGENTA);
  tft.setTextSize(1);
  tft.setCursor(80, 150);
  tft.print("R90");
  
  tft.setCursor(5, 5);
  tft.print("BL:");
  tft.print(brightness);
}

// ===== ТЕМПЕРАТУРА (крупно, ниже потока) =====
void updateTemperatureUI() {
  tft.setCursor(15, 60);
  tft.setTextSize(3);
  
  if (currentTemp == -999.0) {
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
    tft.print("ERR   ");
  } else {
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft.print(currentTemp, 3);
    
    if (currentTemp < 10.0) tft.print("  ");
    else if (currentTemp < 100.0) tft.print(" ");
  }
}

// ===== ЭНКОДЕР (UI) =====
void updateEncoderUI() {
  if (rotationDir != lastRotationDir) {
    tft.fillRect(20, 102, 75, 20, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(25, 105);
    
    if (rotationDir > 0) tft.print("  >>>");
    else if (rotationDir < 0) tft.print("  <<<");
    else tft.print("  STOP");
    
    lastRotationDir = rotationDir;
  }
  
  tft.fillRect(30, 148, 45, 10, ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(5, 150);
  tft.print("ENC: ");
  tft.print(lastPos);
}

// ===== КНОПКА =====
void updateButtonUI() {
  if (buttonPressed) {
    tft.fillRect(100, 100, 25, 25, ST77XX_GREEN);
  } else {
    tft.fillRect(100, 100, 25, 25, ST77XX_RED);
  }
  tft.drawRect(100, 100, 25, 25, ST77XX_WHITE);
}

// ===== МОТОР (крупно, над температурой) =====
void updateMotorUI() {
  tft.fillRect(15, 28, 130, 24, ST77XX_BLACK);
  tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
  tft.setTextSize(3);
  tft.setCursor(15, 30);
  
  if (flowRate < 0) tft.print("-");
  else tft.print(" ");
  
  long v = abs((long)flowRate);
  tft.print(v);
  
  // Выравнивание пробелами, чтобы не оставалось «хвостов»
  if (v < 10)        tft.print("    ");
  else if (v < 100)  tft.print("   ");
  else if (v < 1000) tft.print("  ");
  else if (v < 10000)tft.print(" ");
}

// ===== ЯРКОСТЬ =====
void setBrightness(int value) {
  brightness = constrain(value, 0, 255);
  analogWrite(TFT_BL, brightness);
  
  tft.fillRect(25, 3, 25, 10, ST77XX_BLACK);
  tft.setTextColor(ST77XX_MAGENTA);
  tft.setTextSize(1);
  tft.setCursor(25, 5);
  tft.print(brightness);
}