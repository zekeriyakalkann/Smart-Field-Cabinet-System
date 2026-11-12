// ======================================================
// SMART FIELD CABINET SYSTEM - ANA FIRMWARE
// ======================================================
//
// ESP32 tabanlı saha dolabı izleme ve yerel alarm prototipi.
// Kapak (reed anahtar), eğim (MPU6050), su ve ışık (LDR)
// durumlarını izler. Kritik durumlarda kırmızı LED ve buzzer
// ile uyarı verir, fanı histerezisli olarak açıp kapatır ve
// sistem durumunu SSD1306 OLED ekran ile Serial Monitor
// (115200 baud) üzerinden raporlar.
//
// Sıcaklık değeri MPU6050'nin dahili sensöründen okunur; bu
// değer ortam sıcaklığı değil, MPU6050 yongasının sıcaklığıdır.
//
// Ayrıntılı dokümantasyon: README.md
//
// Dosya bölümleri:
//    1. Kütüphaneler ve Tanımlamalar
//    2. GPIO ve Donanım Yapılandırmaları
//    3. Tür Tanımları ve Global Değişkenler
//    4. Alarm ve LED Yönetimi
//    5. Event Yönetimi
//    6. MPU6050 - Jiroskop ve İvmeölçer İşlemleri
//    7. Kapı Sensörü İşlemleri
//    8. LDR - Işık Sensörü İşlemleri
//    9. Su Sensörü İşlemleri
//   10. Eğim Algılama (MPU6050)
//   11. Sıcaklık ve Fan Kontrolü
//   12. Setup - Başlangıç Yapılandırması
//   13. Loop - Ana Çalışma Döngüsü
//
// OLED ekran işlemleri ayrı bir fonksiyonda bulunmaz: ekran
// setup() içinde başlatılır ve loop() içindeki "OLED"
// bloğunda her döngüde güncellenir.
// ======================================================

// ======================================================
// 1. KÜTÜPHANELER VE TANIMLAMALAR
// ======================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ------------------------------------------------------
// OLED
// ------------------------------------------------------

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ------------------------------------------------------
// MPU6050
// ------------------------------------------------------

#define MPU_ADDR 0x68

// Not: ACCEL_SCALE ve GYRO_SCALE mevcut hesaplamalarda kullanılmaz.
#define ACCEL_SCALE 16384.0
#define GYRO_SCALE 131.0

// ======================================================
// 2. GPIO VE DONANIM YAPILANDIRMALARI
// ======================================================

// ------------------------------------------------------
// SENSOR PINLERI
// ------------------------------------------------------

// Reed anahtar: dijital giriş, HIGH = kapak açık
// LDR ve su sensörü: ADC1 analog girişleri (12 bit, 0-4095)
#define REED_PIN 27
#define LDR_PIN 34
#define WATER_PIN 35

// ------------------------------------------------------
// LED / BUZZER
// ------------------------------------------------------

// Sarı LED tanımlıdır; mevcut sürümde her durumda LOW tutulur.
#define GREEN_LED_PIN 25
#define YELLOW_LED_PIN 26
#define RED_LED_PIN 32
#define BUZZER_PIN 33

// ------------------------------------------------------
// FAN / MOSFET
// ------------------------------------------------------

#define FAN_MOSFET_PIN 14

// Histerezis (°C): sıcaklık >= FAN_ON_TEMP ise fan açılır,
// sıcaklık <= FAN_OFF_TEMP ise fan kapanır.
// Sıcaklık kaynağı MPU6050 yonga sıcaklığıdır, ortam sıcaklığı değildir.
#define FAN_ON_TEMP 27
#define FAN_OFF_TEMP 25.0

// ------------------------------------------------------
// SENSOR EŞİKLERİ
// ------------------------------------------------------

// LDR ve su eşikleri ham ADC değeridir (0-4095).
#define LDR_THRESHOLD 33
#define WATER_THRESHOLD 275

// Eğim eşiği derece cinsindendir (roll veya pitch).
#define TILT_THRESHOLD 15.0

// Durum değişimi için gereken ardışık okuma sayıları
#define SENSOR_CONFIRM_COUNT 5
#define TILT_CONFIRM_COUNT 5

// ======================================================
// 3. TÜR TANIMLARI VE GLOBAL DEĞİŞKENLER
// ======================================================

// ------------------------------------------------------
// EVENT TÜRLERİ
// ------------------------------------------------------

enum EventType {
  EVENT_NONE,

  EVENT_DOOR_OPEN,
  EVENT_DOOR_CLOSED,

  EVENT_CABINET_TILTED,
  EVENT_CABINET_TILT_CLEARED,

  EVENT_LIGHT_HIGH,
  EVENT_LIGHT_NORMAL,

  EVENT_WATER_DETECTED,
  EVENT_WATER_CLEARED,

  EVENT_FAN_STARTED,
  EVENT_FAN_STOPPED
};

// ------------------------------------------------------
// ALARM TÜRLERİ VE ÖNEM SEVİYELERİ
// ------------------------------------------------------

// Etkin alarmlar: UNAUTHORIZED_ACCESS, CABINET_TILTED, HIGH_WATER.
// Diğer alarm türleri tanımlıdır ancak mevcut sürümde tetiklenmez.
enum AlarmType {
  ALARM_NONE,

  ALARM_HIGH_TEMPERATURE,
  ALARM_UNAUTHORIZED_ACCESS,
  ALARM_CABINET_TILTED,
  ALARM_VANDALISM,
  ALARM_HIGH_HUMIDITY,
  ALARM_FAN_FAILURE,
  ALARM_HIGH_WATER
};

enum AlarmSeverity {
  SEVERITY_INFO,
  SEVERITY_WARNING,
  SEVERITY_CRITICAL
};

// ------------------------------------------------------
// ALARM DURUMLARI
// ------------------------------------------------------

bool unauthorizedAccessAlarm = false;
bool cabinetTiltAlarm = false;
bool highWaterAlarm = false;

// ------------------------------------------------------
// SENSOR DURUMLARI
// ------------------------------------------------------

bool doorOpen = false;
bool previousDoorOpen = false;

bool cabinetTilted = false;
int tiltCounter = 0;

bool lightHigh = false;
int lightHighCounter = 0;
int lightNormalCounter = 0;

bool waterDetected = false;
int waterDetectedCounter = 0;
int waterNormalCounter = 0;

// ------------------------------------------------------
// FAN DURUMU
// ------------------------------------------------------

bool fanRunning = false;

// ------------------------------------------------------
// MPU KALİBRASYON VERİLERİ
// ------------------------------------------------------

struct CalibrationData {
  float accX;
  float accY;
  float accZ;

  float gyroX;
  float gyroY;
  float gyroZ;
};

CalibrationData calibration;

// ======================================================
// 4. ALARM VE LED YÖNETİMİ
// ======================================================

// ------------------------------------------------------
// ALARM İSİMLERİ
// ------------------------------------------------------

const char* getAlarmName(AlarmType alarm) {

  switch (alarm) {

    case ALARM_HIGH_TEMPERATURE:
      return "HIGH_TEMPERATURE";

    case ALARM_UNAUTHORIZED_ACCESS:
      return "UNAUTHORIZED_ACCESS";

    case ALARM_CABINET_TILTED:
      return "CABINET_TILTED";

    case ALARM_VANDALISM:
      return "VANDALISM";

    case ALARM_HIGH_HUMIDITY:
      return "HIGH_HUMIDITY";

    case ALARM_FAN_FAILURE:
      return "FAN_FAILURE";

    case ALARM_HIGH_WATER:
      return "HIGH_WATER";

    default:
      return "NONE";
  }
}

// ------------------------------------------------------
// ALARM SEVERITY
// ------------------------------------------------------

AlarmSeverity getAlarmSeverity(AlarmType alarm) {

  switch (alarm) {

    case ALARM_UNAUTHORIZED_ACCESS:
    case ALARM_CABINET_TILTED:
    case ALARM_HIGH_TEMPERATURE:
    case ALARM_VANDALISM:
    case ALARM_HIGH_WATER:

      return SEVERITY_CRITICAL;

    case ALARM_HIGH_HUMIDITY:
    case ALARM_FAN_FAILURE:

      return SEVERITY_WARNING;

    default:

      return SEVERITY_INFO;
  }
}

// ------------------------------------------------------
// AKTÜATÖR YÖNETİMİ (LED / BUZZER)
// ------------------------------------------------------

// Etkin kritik alarm varsa kırmızı LED ve buzzer, yoksa yeşil LED.
void updateActuators() {

  bool criticalAlarm =
    unauthorizedAccessAlarm ||
    cabinetTiltAlarm ||
    highWaterAlarm;

  if (criticalAlarm) {

    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, HIGH);

    digitalWrite(BUZZER_PIN, HIGH);
  }

  else {

    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(YELLOW_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);

    digitalWrite(BUZZER_PIN, LOW);
  }
}

// ------------------------------------------------------
// ALARM SET
// ------------------------------------------------------

void setAlarm(AlarmType alarm, bool active) {

  bool changed = false;

  switch (alarm) {

    case ALARM_UNAUTHORIZED_ACCESS:

      if (unauthorizedAccessAlarm != active) {
        unauthorizedAccessAlarm = active;
        changed = true;
      }

      break;

    case ALARM_CABINET_TILTED:

      if (cabinetTiltAlarm != active) {
        cabinetTiltAlarm = active;
        changed = true;
      }

      break;

    case ALARM_HIGH_WATER:

      if (highWaterAlarm != active) {
        highWaterAlarm = active;
        changed = true;
      }

      break;

    default:
      break;
  }

  if (changed) {

    Serial.print("ALARM ");

    if (active) {
      Serial.print("ACTIVE: ");
    }

    else {
      Serial.print("CLEARED: ");
    }

    Serial.print(getAlarmName(alarm));

    Serial.print(" | SEVERITY: ");

    AlarmSeverity severity = getAlarmSeverity(alarm);

    if (severity == SEVERITY_CRITICAL)
      Serial.println("CRITICAL");

    else if (severity == SEVERITY_WARNING)
      Serial.println("WARNING");

    else
      Serial.println("INFO");
  }

  updateActuators();
}

// ======================================================
// 5. EVENT YÖNETİMİ
// ======================================================

// ------------------------------------------------------
// EVENT İSİMLERİ
// ------------------------------------------------------

const char* getEventName(EventType event) {

  switch (event) {

    case EVENT_DOOR_OPEN:
      return "DOOR_OPEN";

    case EVENT_DOOR_CLOSED:
      return "DOOR_CLOSED";

    case EVENT_CABINET_TILTED:
      return "CABINET_TILTED";

    case EVENT_CABINET_TILT_CLEARED:
      return "CABINET_TILT_CLEARED";

    case EVENT_LIGHT_HIGH:
      return "LIGHT_HIGH";

    case EVENT_LIGHT_NORMAL:
      return "LIGHT_NORMAL";

    case EVENT_WATER_DETECTED:
      return "WATER_DETECTED";

    case EVENT_WATER_CLEARED:
      return "WATER_CLEARED";

    case EVENT_FAN_STARTED:
      return "FAN_STARTED";

    case EVENT_FAN_STOPPED:
      return "FAN_STOPPED";

    default:
      return "NONE";
  }
}

// ------------------------------------------------------
// EVENT PROCESS
// ------------------------------------------------------

// Yalnızca kapak, eğim ve su event'leri alarm durumunu değiştirir.
// LIGHT_* ve FAN_* event'leri yalnızca Serial çıktısına kaydedilir.
void processEvent(EventType event) {

  switch (event) {

    case EVENT_DOOR_OPEN:

      setAlarm(ALARM_UNAUTHORIZED_ACCESS, true);

      break;

    case EVENT_DOOR_CLOSED:

      setAlarm(ALARM_UNAUTHORIZED_ACCESS, false);

      break;

    case EVENT_CABINET_TILTED:

      setAlarm(ALARM_CABINET_TILTED, true);

      break;

    case EVENT_CABINET_TILT_CLEARED:

      setAlarm(ALARM_CABINET_TILTED, false);

      break;

    case EVENT_WATER_DETECTED:

      setAlarm(ALARM_HIGH_WATER, true);

      break;

    case EVENT_WATER_CLEARED:

      setAlarm(ALARM_HIGH_WATER, false);

      break;

    default:
      break;
  }
}

// ------------------------------------------------------
// EVENT GENERATE
// ------------------------------------------------------

void generateEvent(EventType event) {

  Serial.print("EVENT: ");
  Serial.println(getEventName(event));

  processEvent(event);
}

// ======================================================
// 6. MPU6050 - JİROSKOP VE İVMEÖLÇER İŞLEMLERİ
// ======================================================

// ------------------------------------------------------
// MPU6050 REGISTER OKUMA
// ------------------------------------------------------

bool readMPU6050(
  int16_t &ax,
  int16_t &ay,
  int16_t &az,
  int16_t &tempRaw,
  int16_t &gx,
  int16_t &gy,
  int16_t &gz
) {

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom(MPU_ADDR, 14);

  if (Wire.available() < 14) {
    return false;
  }

  ax = Wire.read() << 8 | Wire.read();
  ay = Wire.read() << 8 | Wire.read();
  az = Wire.read() << 8 | Wire.read();

  tempRaw = Wire.read() << 8 | Wire.read();

  gx = Wire.read() << 8 | Wire.read();
  gy = Wire.read() << 8 | Wire.read();
  gz = Wire.read() << 8 | Wire.read();

  return true;
}

// ------------------------------------------------------
// MPU6050 BAŞLAT
// ------------------------------------------------------

void initializeMPU6050() {

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);

  if (Wire.endTransmission() != 0) {

    Serial.println("MPU6050 bulunamadi!");

    while (true) {
      delay(1000);
    }
  }

  // Accelerometer ±2g
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C);
  Wire.write(0x00);
  Wire.endTransmission();

  // Gyroscope ±250 dps
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1B);
  Wire.write(0x00);
  Wire.endTransmission();

  Serial.println("MPU6050 hazir.");
}

// ------------------------------------------------------
// MPU KALİBRASYON
// ------------------------------------------------------

// Açılış konumunu eğim referansı (yaklaşık 0°) olarak kaydeder;
// kalibrasyon süresince sensör hareket ettirilmemelidir.
void calibrateMPU6050() {

  Serial.println();
  Serial.println("MPU6050 kalibrasyonu basliyor...");
  Serial.println("Sensoru sabit tutun.");

  delay(2000);

  long sumAx = 0;
  long sumAy = 0;
  long sumAz = 0;

  long sumGx = 0;
  long sumGy = 0;
  long sumGz = 0;

  const int samples = 1000;

  for (int i = 0; i < samples; i++) {

    int16_t ax, ay, az;
    int16_t tempRaw;
    int16_t gx, gy, gz;

    if (readMPU6050(ax, ay, az, tempRaw, gx, gy, gz)) {

      sumAx += ax;
      sumAy += ay;
      sumAz += az;

      sumGx += gx;
      sumGy += gy;
      sumGz += gz;
    }

    delay(2);
  }

  calibration.accX = sumAx / (float)samples;
  calibration.accY = sumAy / (float)samples;

  // Z ekseninde yaklaşık +1g varsayımı
  calibration.accZ =
    (sumAz / (float)samples) - 16384.0;

  calibration.gyroX =
    sumGx / (float)samples;

  calibration.gyroY =
    sumGy / (float)samples;

  calibration.gyroZ =
    sumGz / (float)samples;

  Serial.println("Kalibrasyon tamamlandi.");
}

// ======================================================
// 7. KAPI SENSÖRÜ İŞLEMLERİ
// ======================================================

bool readDoorState() {

  return digitalRead(REED_PIN) == HIGH;
}

// ======================================================
// 8. LDR - IŞIK SENSÖRÜ İŞLEMLERİ
// ======================================================

// ------------------------------------------------------
// LDR OKUMA
// ------------------------------------------------------

int readLDR() {

  return analogRead(LDR_PIN);
}

// ------------------------------------------------------
// LDR DURUMU
// ------------------------------------------------------

void updateLightState() {

  int value = readLDR();

  if (value > LDR_THRESHOLD) {

    lightHighCounter++;
    lightNormalCounter = 0;

    if (!lightHigh &&
        lightHighCounter >= SENSOR_CONFIRM_COUNT) {

      lightHigh = true;

      generateEvent(EVENT_LIGHT_HIGH);
    }
  }

  else {

    lightNormalCounter++;
    lightHighCounter = 0;

    if (lightHigh &&
        lightNormalCounter >= SENSOR_CONFIRM_COUNT) {

      lightHigh = false;

      generateEvent(EVENT_LIGHT_NORMAL);
    }
  }
}

// ======================================================
// 9. SU SENSÖRÜ İŞLEMLERİ
// ======================================================

// ------------------------------------------------------
// WATER SENSOR
// ------------------------------------------------------

int readWaterSensor() {

  return analogRead(WATER_PIN);
}

// ------------------------------------------------------
// WATER DURUMU
// ------------------------------------------------------

void updateWaterState() {

  int value = readWaterSensor();

  if (value > WATER_THRESHOLD) {

    waterDetectedCounter++;
    waterNormalCounter = 0;

    if (!waterDetected &&
        waterDetectedCounter >= SENSOR_CONFIRM_COUNT) {

      waterDetected = true;

      generateEvent(EVENT_WATER_DETECTED);
    }
  }

  else {

    waterNormalCounter++;
    waterDetectedCounter = 0;

    if (waterDetected &&
        waterNormalCounter >= SENSOR_CONFIRM_COUNT) {

      waterDetected = false;

      generateEvent(EVENT_WATER_CLEARED);
    }
  }
}

// ======================================================
// 10. EĞİM ALGILAMA (MPU6050)
// ======================================================

// Eğim, TILT_CONFIRM_COUNT ardışık okumayla doğrulanır;
// eşik altındaki ilk okumada eğim durumu temizlenir.
void updateTiltState(float roll, float pitch) {

  bool tiltDetected =
    abs(roll) > TILT_THRESHOLD ||
    abs(pitch) > TILT_THRESHOLD;

  if (tiltDetected) {

    tiltCounter++;

    if (!cabinetTilted &&
        tiltCounter >= TILT_CONFIRM_COUNT) {

      cabinetTilted = true;

      generateEvent(EVENT_CABINET_TILTED);
    }
  }

  else {

    tiltCounter = 0;

    if (cabinetTilted) {

      cabinetTilted = false;

      generateEvent(EVENT_CABINET_TILT_CLEARED);
    }
  }
}

// ======================================================
// 11. SICAKLIK VE FAN KONTROLÜ
// ======================================================

void updateFan(float temperature) {

  bool previousFanState = fanRunning;

  // Fanı aç
  if (!fanRunning && temperature >= FAN_ON_TEMP) {

    fanRunning = true;
  }

  // Fanı kapat
  else if (fanRunning && temperature <= FAN_OFF_TEMP) {

    fanRunning = false;
  }

  // MOSFET kontrolü
  digitalWrite(
    FAN_MOSFET_PIN,
    fanRunning ? HIGH : LOW
  );

  // Fan durumu değiştiyse event oluştur
  if (fanRunning != previousFanState) {

    if (fanRunning) {

      generateEvent(EVENT_FAN_STARTED);
    }

    else {

      generateEvent(EVENT_FAN_STOPPED);
    }
  }
}

// ======================================================
// 12. SETUP - BAŞLANGIÇ YAPILANDIRMASI
// ======================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("=================================");
  Serial.println(" SMART FIELD CABINET SYSTEM");
  Serial.println("=================================");
  Serial.println();

  // ----------------------------------------------------
  // I2C
  // ----------------------------------------------------

  Wire.begin(21, 22);

  // ----------------------------------------------------
  // REED
  // ----------------------------------------------------

  pinMode(REED_PIN, INPUT);

  // ----------------------------------------------------
  // ADC
  // ----------------------------------------------------

  analogReadResolution(12);

  // ----------------------------------------------------
  // LED / BUZZER
  // ----------------------------------------------------

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // ----------------------------------------------------
  // FAN MOSFET
  // ----------------------------------------------------

  pinMode(FAN_MOSFET_PIN, OUTPUT);

  // Başlangıçta fan kapalı
  digitalWrite(FAN_MOSFET_PIN, LOW);

  fanRunning = false;

  // ----------------------------------------------------
  // OLED
  // ----------------------------------------------------

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDR)) {

    Serial.println("OLED bulunamadi!");

    while (true) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("SMART CABINET");

  display.setCursor(0, 15);
  display.println("System starting...");

  display.display();

  // ----------------------------------------------------
  // MPU
  // ----------------------------------------------------

  initializeMPU6050();

  calibrateMPU6050();

  // ----------------------------------------------------
  // İLK KAPI DURUMU
  // ----------------------------------------------------

  doorOpen = readDoorState();
  previousDoorOpen = doorOpen;

  // ----------------------------------------------------
  // AKTÜATÖRLER
  // ----------------------------------------------------

  updateActuators();

  Serial.println();
  Serial.println("SYSTEM READY");
  Serial.println();
}

// ======================================================
// 13. LOOP - ANA ÇALIŞMA DÖNGÜSÜ
// ======================================================

void loop() {

  // ====================================================
  // DOOR
  // ====================================================

  doorOpen = readDoorState();

  if (doorOpen != previousDoorOpen) {

    if (doorOpen) {

      generateEvent(EVENT_DOOR_OPEN);
    }

    else {

      generateEvent(EVENT_DOOR_CLOSED);
    }

    previousDoorOpen = doorOpen;
  }

  // ====================================================
  // LDR
  // ====================================================

  updateLightState();

  // ====================================================
  // WATER
  // ====================================================

  updateWaterState();

  // ====================================================
  // MPU
  // ====================================================

  int16_t ax, ay, az;
  int16_t tempRaw;
  int16_t gx, gy, gz;

  float temperature = 0;
  float roll = 0;
  float pitch = 0;

  if (readMPU6050(
        ax,
        ay,
        az,
        tempRaw,
        gx,
        gy,
        gz)) {

    // -----------------------------------------------
    // Calibration
    // -----------------------------------------------

    float calibratedAx =
      ax - calibration.accX;

    float calibratedAy =
      ay - calibration.accY;

    float calibratedAz =
      az - calibration.accZ;

    // -----------------------------------------------
    // Temperature
    // MPU6050 yonga sıcaklığı (ortam sıcaklığı değil)
    // -----------------------------------------------

    temperature =
      (tempRaw / 340.0) + 36.53;

    // -----------------------------------------------
    // Tilt calculation
    // -----------------------------------------------

    roll =
      atan2(
        calibratedAy,
        calibratedAz
      ) * 180.0 / PI;

    pitch =
      atan2(
        -calibratedAx,
        sqrt(
          calibratedAy * calibratedAy +
          calibratedAz * calibratedAz
        )
      ) * 180.0 / PI;

    // -----------------------------------------------
    // Tilt state
    // -----------------------------------------------

    updateTiltState(roll, pitch);

    // -----------------------------------------------
    // FAN
    // -----------------------------------------------

    updateFan(temperature);
  }

  // ====================================================
  // ACTUATORS
  // ====================================================

  updateActuators();

  // ====================================================
  // SERIAL MONITOR
  // ====================================================

  Serial.print("TEMP: ");
  Serial.print(temperature, 1);

  Serial.print(" C | ");

  Serial.print("ROLL: ");
  Serial.print(roll, 1);

  Serial.print(" | PITCH: ");
  Serial.print(pitch, 1);

  Serial.print(" | DOOR: ");
  Serial.print(doorOpen ? "OPEN" : "CLOSED");

  Serial.print(" | LDR: ");
  Serial.print(lightHigh ? "HIGH" : "NORMAL");

  Serial.print(" | WATER: ");
  Serial.print(waterDetected ? "DETECTED" : "NORMAL");

  Serial.print(" | TILT: ");
  Serial.print(cabinetTilted ? "YES" : "NO");

  Serial.print(" | FAN: ");
  Serial.println(fanRunning ? "ON" : "OFF");

  // ====================================================
  // OLED
  // ====================================================

  display.clearDisplay();

  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("SMART CABINET");

  display.setCursor(0, 10);
  display.print("R:");
  display.print(roll, 1);

  display.print(" P:");
  display.print(pitch, 1);

  display.setCursor(0, 20);
  display.print("TEMP:");
  display.print(temperature, 1);
  display.println(" C");

  display.setCursor(0, 30);
  display.print("DOOR:");
  display.println(
    doorOpen ? "OPEN" : "CLOSED"
  );

  display.setCursor(0, 40);
  display.print("LIGHT:");
  display.println(
    lightHigh ? "HIGH" : "NORMAL"
  );

  display.setCursor(0, 50);
  display.print("W:");
  display.print(
    waterDetected ? "DET" : "OK"
  );

  display.print(" T:");
  display.print(
    cabinetTilted ? "YES" : "NO"
  );

  display.print(" F:");
  display.print(
    fanRunning ? "ON" : "OFF"
  );

  display.display();

  // ====================================================
  // LOOP DELAY
  // ====================================================

  delay(100);
}