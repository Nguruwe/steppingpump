#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_MAX31865.h>
#include <Encoder.h>

// ===== ПИНЫ ДИСПЛЕЯ =====
#define TFT_CS    10
#define TFT_RST   8
#define TFT_DC    9
#define TFT_BL    5     // Пин подсветки (ШИМ)

// ===== ПИНЫ ЭНКОДЕРА =====
#define ENC_CLK   2
#define ENC_DT    3
#define ENC_SW    4

// ===== ПИНЫ ДРАЙВЕРА ШАГОВОГО МОТОРА =====
#define DRV_EN    15   // A1
#define DRV_STEP  16   // A2
#define DRV_DIR   17   // A3

// ===== ПИНЫ И НАСТРОЙКИ MAX31865 =====
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
bool lastButtonState = HIGH;   // <-- ДОБАВИТЬ ЭТУ СТРОКУ
int rotationDir = 0;

// ===== ПЕРЕМЕННЫЕ МОТОРА =====
bool motorEnabled = false;          // Состояние драйвера (вкл/выкл)
long motorSpeed = 0;                // Скорость: положительная — вперёд, отрицательная — назад, 0 — стоп
unsigned long lastStepTime = 0;     // Время последнего шага
unsigned long stepInterval = 0;     // Интервал между шагами (мкс)

// ===== ПЕРЕМЕННЫЕ ТЕМПЕРАТУРЫ =====
float currentTemp = 0.0;
float lastTemp = -999.0;
unsigned long lastTempUpdate = 0;

// ===== ЯРКОСТЬ =====
int brightness = 32;

void setup() {
  Serial.begin(9600);
  
  // Инициализация дисплея
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  
  // Инициализация датчика температуры
  thermo.begin(MAX31865_4WIRE);
  
  // Настройка подсветки
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, brightness);
  
  // Настройка кнопки энкодера
  pinMode(ENC_SW, INPUT_PULLUP);
  
  // Настройка пинов драйвера
  pinMode(DRV_EN, OUTPUT);
  pinMode(DRV_STEP, OUTPUT);
  pinMode(DRV_DIR, OUTPUT);
  
  // Драйвер выключен (EN активен низким уровнем — HIGH = выключен)
  digitalWrite(DRV_EN, HIGH);
  digitalWrite(DRV_STEP, LOW);
  digitalWrite(DRV_DIR, LOW);
  
  tft.fillScreen(ST77XX_BLACK);
  
  Serial.println("=== TFT + Энкодер + MAX31865 + DRV8825 ===");
  
  drawStaticInterface();
  updateTemperatureUI();
  updateEncoderUI();
  updateButtonUI();
  updateMotorUI();
}

void loop() {
  // ----- 1. ЭНКОДЕР: УПРАВЛЕНИЕ СКОРОСТЬЮ -----
  int newPos = myEncoder.read() / 4;
  if (newPos != lastPos) {
    int delta = newPos - lastPos;
    
    // Меняем скорость в зависимости от поворота
    // Чем больше повернули — тем быстрее
    motorSpeed += delta * 5;   // Шаг изменения скорости
    motorSpeed = constrain(motorSpeed, -500, 500);
    
    if (motorSpeed > 0) rotationDir = 1;
    else if (motorSpeed < 0) rotationDir = -1;
    else rotationDir = 0;
    
    // Пересчёт интервала шага (мкс). Чем больше скорость, тем меньше интервал.
    if (motorSpeed == 0) {
      stepInterval = 0;
    } else {
      // 500 — макс. скорость (интервал 400 мкс), 1 — мин. скорость (интервал 20000 мкс)
      stepInterval = map(abs(motorSpeed), 1, 500, 20000, 400);
    }
    
    lastPos = newPos;
    updateEncoderUI();
    updateMotorUI();
  }

  // ----- 2. КНОПКА: ВКЛ/ВЫКЛ ДРАЙВЕРА -----
  bool currentButtonState = digitalRead(ENC_SW);
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressed = !buttonPressed;
    motorEnabled = buttonPressed;
    
    if (motorEnabled) {
      digitalWrite(DRV_EN, LOW);   // Включаем драйвер
      Serial.println("Motor ON");
    } else {
      digitalWrite(DRV_EN, HIGH);  // Выключаем драйвер
      motorSpeed = 0;              // Сбрасываем скорость
      stepInterval = 0;
      Serial.println("Motor OFF");
    }
    
    updateButtonUI();
    updateMotorUI();
    delay(50); // Дебаунс
  }
  lastButtonState = currentButtonState;

  // ----- 3. ГЕНЕРАЦИЯ ШАГОВ (БЕЗ delay) -----
  if (motorEnabled && motorSpeed != 0 && stepInterval > 0) {
    unsigned long now = micros();
    if (now - lastStepTime >= stepInterval) {
      lastStepTime = now;
      
      // Направление
      digitalWrite(DRV_DIR, motorSpeed > 0 ? HIGH : LOW);
      
      // Импульс шага
      digitalWrite(DRV_STEP, HIGH);
      delayMicroseconds(5);   // Минимальная длительность импульса
      digitalWrite(DRV_STEP, LOW);
    }
  }

  // ----- 4. ТЕМПЕРАТУРА -----
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
    
    if (currentTemp < 10.0) {
      tft.print("  ");
    } else if (currentTemp < 100.0) {
      tft.print(" ");
    }
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

// ===== МОТОР (ИНФО НА ЭКРАНЕ) =====
void updateMotorUI() {
  tft.fillRect(80, 70, 45, 10, ST77XX_BLACK);
  tft.setTextColor(ST77XX_ORANGE);
  tft.setTextSize(1);
  tft.setCursor(80, 72);
  tft.print("V:");
  tft.print(motorSpeed);
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