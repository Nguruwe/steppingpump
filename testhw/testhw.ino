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

// ===== ПИНЫ И НАСТРОЙКИ MAX31865 =====
#define MAX_CS    7     // Наш свободный пин для CS температурного модуля
#define RREF      4300.0  // Наш новый впаянный резистор 1206 на 4.3 кОм
#define RNOMINAL  1000.0  // Номинал датчика PT1000 при 0°C

// ===== ОБЪЕКТЫ =====
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Encoder myEncoder(ENC_CLK, ENC_DT);

// Используем аппаратный SPI для MAX31865 (передаем только пин CS)
// Он автоматически займет общие с дисплеем пины D11 (MOSI) и D13 (SCK), а также свободный D12 (MISO)
#define MAX_CS   7
#define MAX_SDI  6   // Перенесли с 11
#define MAX_SDO  12  // Оставили на 12
#define MAX_CLK  14  // Перенесли с 13 на пин A0 (цифровой 14)

// Инициализация программного SPI: (CS, MOSI, MISO, CLK)
Adafruit_MAX31865 thermo = Adafruit_MAX31865(MAX_CS, MAX_SDI, MAX_SDO, MAX_CLK);

// ===== ПЕРЕМЕННЫЕ =====
int lastRotationDir = 0;   // Чтобы стрелки не мерцали, если направление не менялось
bool lastButtonPressed = false; // Чтобы квадрат кнопки не мерцал

int lastPos = 0;
bool lastButtonState = HIGH;
bool buttonPressed = false;
int rotationDir = 0;
int brightness = 32;

float currentTemp = 0.0;
float lastTemp = -999.0; // Для отслеживания изменений температуры
unsigned long lastTempUpdate = 0;

void setup() {
  Serial.begin(9600);
  
  // Инициализация дисплея и поворот на 90°
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // 90° по часовой стрелке
  
  // Инициализация датчика температуры в режиме 4-проводного подключения
  thermo.begin(MAX31865_4WIRE);
  
  // Настройка подсветки (ШИМ)
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, brightness);
  
  // Настройка кнопки энкодера
  pinMode(ENC_SW, INPUT_PULLUP);
  
  tft.fillScreen(ST77XX_BLACK);
  
  Serial.println("=== TFT + Энкодер + MAX31865 PT1000 тест ===");
  
  // Рисуем интерфейс (один раз статические элементы)
  drawStaticInterface();
  updateTemperatureUI();
  updateEncoderUI();
  updateButtonUI();
}

void loop() {
  // ----- 1. ЧИТАЕМ И ОБНОВЛЯЕМ ЭНКОДЕР -----
  int newPos = myEncoder.read() / 4;
  if (newPos != lastPos) {
    int delta = newPos - lastPos;
    if (delta > 0) rotationDir = -1;  // Влево
    else rotationDir = 1;              // Вправо
    
    lastPos = newPos;
    
    // Обновляем НА ЭКРАНЕ только данные энкодера, температуру не трогаем!
    updateEncoderUI();
  }

  // ----- 2. ЧИТАЕМ И ОБНОВЛЯЕМ КНОПКУ -----
  bool currentButtonState = digitalRead(ENC_SW);
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressed = !buttonPressed;
    
    // Обновляем НА ЭКРАНЕ только кнопку!
    updateButtonUI();
    delay(50); // Дебаунс
  }
  lastButtonState = currentButtonState;

  // ----- 3. ЧИТАЕМ И ОБНОВЛЯЕМ ТЕМПЕРАТУРУ -----
  if (millis() - lastTempUpdate >= 500) {
    lastTempUpdate = millis();
    
    // Перед чтением сбрасываем старые ошибки, которые могли прилететь от переходного процесса
    thermo.clearFault(); 
    // Читаем температуру
    float tempReading = thermo.temperature(RNOMINAL, RREF);
    
    uint8_t fault = thermo.readFault();
    if (fault) {
      currentTemp = -999.0;
      thermo.clearFault();
    } else {
      currentTemp = tempReading;
    }
    
    // Если температура РЕАЛЬНО изменилась — обновляем НА ЭКРАНЕ только её!
    if (currentTemp != lastTemp) {
      updateTemperatureUI();
      lastTemp = currentTemp;
    }
  }
}

// ===== ОТРИСОВКА СТАТИКИ (вызывается один раз в setup) =====
void drawStaticInterface() {
  // Рамка
  tft.drawRect(0, 0, tft.width(), tft.height(), ST77XX_WHITE);
  
  // Буква "С" для температуры
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(115, 60);
  tft.print("C");
  
  // Статичный текст внизу
  tft.setTextColor(ST77XX_MAGENTA);
  tft.setTextSize(1);
  tft.setCursor(80, 150);
  tft.print("R90");
  
  tft.setCursor(5, 5);
  tft.print("BL:");
  tft.print(brightness);
}

// ===== ОБНОВЛЕНИЕ ДИНАМИЧЕСКИХ ДАННЫХ (без мерцания всего экрана) =====
// ===== 1. ОБНОВЛЕНИЕ ТОЛЬКО ТЕМПЕРАТУРЫ =====
void updateTemperatureUI() {
  tft.setCursor(15, 55);
  tft.setTextSize(3);
  
  if (currentTemp == -999.0) {
    // Выводим ошибку и принудительно затираем пробелами всё оставшееся место
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK); 
    tft.print("ERR   "); 
  } else {
    tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); 
    tft.print(currentTemp, 3); 
    
    // Выравниваем длину строки пробелами:
    // Если температура < 10.00 (на экране 4 символа, например 8.54), добавляем 2 пробела
    if (currentTemp < 10.0) {
      tft.print("  ");
    }
    // Если температура < 100.00 (на экране 5 символов, например 24.35), добавляем 1 пробел
    else if (currentTemp < 100.0) {
      tft.print(" ");
    }
    // Если 100.00 и выше (6 символов), пробелы не нужны — строка и так максимальной длины
  }
}
// ===== 2. ОБНОВЛЕНИЕ ТОЛЬКО ДАННЫХ ЭНКОДЕРА =====
void updateEncoderUI() {
  // Стрелки направления обновляем только если направление РЕАЛЬНО изменилось
  if (rotationDir != lastRotationDir) {
    tft.fillRect(20, 102, 75, 20, ST77XX_BLACK); // Затираем только стрелку
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(25, 105);
    
    if (rotationDir > 0) tft.print("  >>>");
    else if (rotationDir < 0) tft.print("  <<<");
    else tft.print("  STOP");
    
    lastRotationDir = rotationDir;
  }
  
  // Цифры позиции энкодера (затираем и пишем заново только само число)
  tft.fillRect(30, 148, 45, 10, ST77XX_BLACK); 
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(5, 150);
  tft.print("ENC: ");
  tft.print(lastPos);
}

// ===== 3. ОБНОВЛЕНИЕ ТОЛЬКО КВАДРАТА КНОПКИ =====
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

// ===== ФУНКЦИЯ УПРАВЛЕНИЯ ЯРКОСТЬЮ =====
void setBrightness(int value) {
  brightness = constrain(value, 0, 255);
  analogWrite(TFT_BL, brightness);
  
  // Обновляем циферки яркости в углу
  tft.fillRect(25, 3, 25, 10, ST77XX_BLACK);
  tft.setTextColor(ST77XX_MAGENTA);
  tft.setTextSize(1);
  tft.setCursor(25, 5);
  tft.print(brightness);
}