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
#define DRV_EN    15   // A1
#define DRV_STEP  16   // A2
#define DRV_DIR   17   // A3

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
bool lastButtonPressed = false;
bool buttonPressed = false;
bool lastButtonState = HIGH;
int rotationDir = 0;

// ===== ПЕРЕМЕННЫЕ МОТОРА =====
volatile bool motorEnabled = false;
volatile long motorSpeed = 0;          // -500..+500, знак = направление
volatile unsigned long stepInterval = 0;  // мкс между шагами
volatile bool stepState = false;       // для генерации короткого импульса

// ===== НАСТРОЙКИ СКОРОСТИ =====
const long MAX_SPEED = 2000;            // верхняя граница «единиц»
const unsigned long MIN_INTERVAL = 50;  // мкс, самый быстрый шаг (20 кГц)
const unsigned long MAX_INTERVAL = 20000; // мкс, самый медленный шаг

// ===== ТЕМПЕРАТУРА =====
float currentTemp = 0.0;
float lastTemp = -999.0;
unsigned long lastTempUpdate = 0;

// ===== ЯРКОСТЬ =====
int brightness = 32;

// ===== ISR ТАЙМЕРА =====
// Каждое срабатывание таймера формирует один импульс STEP.
// Импульс делается в два захода: HIGH на одном срабатывании, LOW на следующем.
// Благодаря этому длительность импульса равна периоду таймера и не блокирует loop().
void stepISR() {
  if (!motorEnabled || motorSpeed == 0 || stepInterval == 0) {
    digitalWrite(DRV_STEP, LOW);
    stepState = false;
    return;
  }
  
  if (!stepState) {
    // Направление выставляем перед импульсом
    digitalWrite(DRV_DIR, (motorSpeed > 0) ? HIGH : LOW);
    digitalWrite(DRV_STEP, HIGH);
    stepState = true;
  } else {
    digitalWrite(DRV_STEP, LOW);
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
  
  pinMode(DRV_EN, OUTPUT);
  pinMode(DRV_STEP, OUTPUT);
  pinMode(DRV_DIR, OUTPUT);
  
  digitalWrite(DRV_EN, HIGH);   // выключен
  digitalWrite(DRV_STEP, LOW);
  digitalWrite(DRV_DIR, LOW);
  
  // Инициализация Timer1: 1000 мкс = 1 кГц, при запуске мотор выключен
  Timer1.initialize(1000);
  Timer1.attachInterrupt(stepISR);
  
  tft.fillScreen(ST77XX_BLACK);
  
  Serial.println("=== TFT + Энкодер + MAX31865 + DRV8825 (Timer1) ===");
  
  drawStaticInterface();
  updateTemperatureUI();
  updateEncoderUI();
  updateButtonUI();
  updateMotorUI();
}

void loop() {
  // ----- 1. ЭНКОДЕР -----
  int newPos = myEncoder.read() / 4;
  if (newPos != lastPos) {
    int delta = newPos - lastPos;
    
    // Временно разрешаем менять скорость 
    noInterrupts();
    motorSpeed += delta * 5;
    motorSpeed = constrain(motorSpeed, -MAX_SPEED, MAX_SPEED);

    long spd = motorSpeed;
    if (spd > 0) rotationDir = 1;
    else if (spd < 0) rotationDir = -1;
    else rotationDir = 0;

    if (spd == 0) {
      stepInterval = 0;
    } else {
      unsigned long interval = map(abs(spd), 1, MAX_SPEED, MAX_INTERVAL, MIN_INTERVAL);
      stepInterval = interval;
      Timer1.setPeriod(interval);
    }
    
    lastPos = newPos;
    updateEncoderUI();
    updateMotorUI();
  }

  // ----- 2. КНОПКА -----
  bool currentButtonState = digitalRead(ENC_SW);
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressed = !buttonPressed;
    
    noInterrupts();
    motorEnabled = buttonPressed;
    interrupts();
    
    if (buttonPressed) {
      digitalWrite(DRV_EN, LOW);
      Serial.println("Motor ON");
    } else {
      digitalWrite(DRV_EN, HIGH);
      noInterrupts();
      motorSpeed = 0;
      stepInterval = 0;
      interrupts();
      digitalWrite(DRV_STEP, LOW);
      Serial.println("Motor OFF");
    }
    
    updateButtonUI();
    updateMotorUI();
    delay(50); // дебаунс кнопки — теперь безопасен, т.к. шаги идут в ISR
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
  
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(115, 60);
  tft.print("C");
  
  tft.setTextColor(ST77XX_MAGENTA);
  tft.setTextSize(1);
  tft.setCursor(80, 150);
  tft.print("R90");
  
  tft.setCursor(5, 5);
  tft.print("BL:");
  tft.print(brightness);
}

// ===== ТЕМПЕРАТУРА =====
void updateTemperatureUI() {
  tft.setCursor(15, 55);
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

// ===== ЭНКОДЕР =====
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
  if (buttonPressed != lastButtonPressed) {
    if (buttonPressed) {
      tft.fillRect(100, 100, 25, 25, ST77XX_GREEN);
    } else {
      tft.fillRect(100, 100, 25, 25, ST77XX_RED);
    }
    tft.drawRect(100, 100, 25, 25, ST77XX_WHITE);
    lastButtonPressed = buttonPressed;
  }
}

// ===== МОТОР =====
void updateMotorUI() {
  tft.fillRect(80, 70, 45, 10, ST77XX_BLACK);
  tft.setTextColor(ST77XX_ORANGE);
  tft.setTextSize(1);
  tft.setCursor(80, 72);
  tft.print("V:");
  tft.print((long)motorSpeed);
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