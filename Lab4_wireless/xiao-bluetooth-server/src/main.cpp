#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <stdlib.h>

// ====================== BLE ======================
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;

bool deviceConnected = false;
bool oldDeviceConnected = false;

unsigned long previousMillis = 0;
const long interval = 1000;  // print/send interval (ms)

// Print device name periodically so it is guaranteed to appear in your screenshot
unsigned long namePrintMillis = 0;
const long namePrintInterval = 5000;

// Server device name (will show in Serial Monitor)
static const char* SERVER_NAME = "BLE_SERVER";

// UUIDs
#define SERVICE_UUID        "724fc8e5-485e-467c-a7b9-ef2796515386"
#define CHARACTERISTIC_UUID "976e3398-600d-4d49-ac5d-95383f1c14da"

// ====================== HC-SR04 Pins ======================
static const int TRIG_PIN = 4; 
static const int ECHO_PIN = 5;

// ====================== DSP: Moving Average ======================
static const int MA_WINDOW = 5;
float maBuffer[MA_WINDOW];
int maIndex = 0;
int maCount = 0;

float rawDistanceCm = NAN;
float denoisedDistanceCm = NAN;

// ====================== BLE Callbacks ======================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.print("Client connected to ");
    Serial.println(SERVER_NAME);
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.print("Client disconnected from ");
    Serial.println(SERVER_NAME);
  }
};

// ====================== HC-SR04 Reading ======================
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Timeout avoids blocking forever
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return NAN;

  // Distance(cm) = duration(us) * 0.0343 / 2
  return (duration * 0.0343f) / 2.0f;
}

// ====================== DSP Algorithm: Moving Average ======================
float movingAverage(float x) {
  if (!isnan(x)) {
    maBuffer[maIndex] = x;
    maIndex = (maIndex + 1) % MA_WINDOW;
    if (maCount < MA_WINDOW) maCount++;
  } else {
    if (maCount == 0) return NAN;
  }

  float sum = 0.0f;
  for (int i = 0; i < maCount; i++) sum += maBuffer[i];
  return (maCount > 0) ? (sum / maCount) : NAN;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  delay(1000);
  Serial.println("Starting BLE work!");
  Serial.print("Server Device Name: ");
  Serial.println(SERVER_NAME);

  // HC-SR04 pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // Init moving average buffer
  for (int i = 0; i < MA_WINDOW; i++) maBuffer[i] = 0.0f;

  // BLE init
  BLEDevice::init(SERVER_NAME);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setValue("Ready");

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);

  BLEDevice::startAdvertising();

  Serial.println("Advertising started.");
  Serial.println("Characteristic defined.");
  Serial.println("Output: raw_cm, denoised_cm, BLE sent/not sent");
}

void loop() {
  // Periodically print server name (helps screenshot)
  unsigned long now = millis();
  if (now - namePrintMillis >= namePrintInterval) {
    namePrintMillis = now;
    Serial.print("Server Device Name: ");
    Serial.println(SERVER_NAME);
  }

  // Read + DSP + print + conditional BLE transmit once per interval
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    rawDistanceCm = readDistanceCm();
    denoisedDistanceCm = movingAverage(rawDistanceCm);

    // Print raw and denoised
    Serial.print("raw_cm=");
    if (isnan(rawDistanceCm)) Serial.print("NaN");
    else Serial.print(rawDistanceCm, 2);

    Serial.print(" | denoised_cm=");
    if (isnan(denoisedDistanceCm)) Serial.print("NaN");
    else Serial.print(denoisedDistanceCm, 2);

    bool shouldSend = (!isnan(denoisedDistanceCm) && denoisedDistanceCm < 30.0f);

if (deviceConnected && shouldSend) {
      // Send only the denoised distance value as a float
      char payload[16];
      snprintf(payload, sizeof(payload), "%.2f", denoisedDistanceCm);

      pCharacteristic->setValue(payload);
      pCharacteristic->notify();

      Serial.print(" | BLE sent: ");
      Serial.println(payload);
    } else {
      Serial.print(" | BLE not sent");
      if (!deviceConnected) Serial.print(" (no client)");
      else Serial.print(" (>=30cm)");
      Serial.println();
    }
  }

  // reconnect advertising when disconnected
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("Start advertising again");
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  delay(10);
}