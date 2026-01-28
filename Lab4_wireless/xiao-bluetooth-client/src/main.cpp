#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// TODO: change these UUIDs to match your server
static BLEUUID serviceUUID("724fc8e5-485e-467c-a7b9-ef2796515386");
static BLEUUID charUUID("976e3398-600d-4d49-ac5d-95383f1c14da");

static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;

static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static BLEAdvertisedDevice* myDevice = nullptr;

// Store server name
static String serverName = "Unknown";

// ====================== Statistics for Max/Min ======================
static float currentDistance = NAN;
static float maxDistance = -999999.0f;  // Initialize to very small value
static float minDistance = 999999.0f;   // Initialize to very large value
static int dataReceivedCount = 0;       // Count received data

// ====================== Notify Callback: Process received data ======================
static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {

  // Convert received data to string
  String receivedData = "";
  for (size_t i = 0; i < length; i++) {
    receivedData += (char)pData[i];
  }

  dataReceivedCount++;

  Serial.println("===========================================");
  Serial.print("Data #");
  Serial.print(dataReceivedCount);
  Serial.print(" received from ");
  Serial.println(serverName);
  Serial.print("Raw data: ");
  Serial.println(receivedData);

  // Parse as pure number (denoised distance)
  currentDistance = receivedData.toFloat();

  // Check if valid data
  if (currentDistance > 0.0f) {
    // Update maximum and minimum
    if (currentDistance > maxDistance) {
      maxDistance = currentDistance;
    }
    if (currentDistance < minDistance) {
      minDistance = currentDistance;
    }

    // Print current, max, and min values
    Serial.println("-------------------------------------------");
    Serial.print("Current Distance: ");
    Serial.print(currentDistance, 2);
    Serial.println(" cm");
    
    Serial.print("Maximum Distance: ");
    Serial.print(maxDistance, 2);
    Serial.println(" cm");
    
    Serial.print("Minimum Distance: ");
    Serial.print(minDistance, 2);
    Serial.println(" cm");
    
    Serial.println("===========================================");
    Serial.println();
  } else {
    Serial.println("Warning: Invalid distance data received");
    Serial.println("===========================================");
    Serial.println();
  }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) override {
    Serial.print("Client connected to ");
    Serial.println(serverName);
  }

  void onDisconnect(BLEClient* pclient) override {
    connected = false;
    Serial.print("Disconnected from ");
    Serial.println(serverName);
    
    // Print final statistics
    Serial.println("===========================================");
    Serial.println("Final Statistics:");
    Serial.print("Total data received: ");
    Serial.println(dataReceivedCount);
    
    if (maxDistance > -999999.0f && minDistance < 999999.0f) {
      Serial.print("Maximum Distance: ");
      Serial.print(maxDistance, 2);
      Serial.println(" cm");
      Serial.print("Minimum Distance: ");
      Serial.print(minDistance, 2);
      Serial.println(" cm");
    } else {
      Serial.println("No valid data received");
    }
    Serial.println("===========================================");
  }
};

bool connectToServer() {
  Serial.print("Forming a connection to ");
  Serial.print(serverName);
  Serial.print(" | ");
  Serial.println(myDevice->getAddress().toString().c_str());

  BLEClient* pClient = BLEDevice::createClient();
  Serial.println(" - Created client");
  pClient->setClientCallbacks(new MyClientCallback());

  // Connect to the BLE Server
  if (!pClient->connect(myDevice)) {
    Serial.println(" - Connect failed");
    return false;
  }

  Serial.print("Connected to server: ");
  Serial.print(serverName);
  Serial.print(" (");
  Serial.print(myDevice->getAddress().toString().c_str());
  Serial.println(")");

  pClient->setMTU(517);

  // Get service
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service");

  // Get characteristic
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.print("Failed to find characteristic UUID: ");
    Serial.println(charUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our characteristic");

  // Read initial value
  if (pRemoteCharacteristic->canRead()) {
    std::string value = pRemoteCharacteristic->readValue();
    Serial.print("Initial value from ");
    Serial.print(serverName);
    Serial.print(": ");
    Serial.println(value.c_str());
  }

  // Enable notify
  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    Serial.print("Notify enabled for ");
    Serial.println(serverName);
    Serial.println("===========================================");
    Serial.println("Waiting for distance data...");
    Serial.println("===========================================");
  }

  connected = true;
  
  // Reset statistics
  dataReceivedCount = 0;
  maxDistance = -999999.0f;
  minDistance = 999999.0f;
  
  return true;
}

/**
 * Scan for BLE servers and find the first one advertising the service we want.
 */
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      // Capture server name for screenshot
      if (advertisedDevice.haveName()) {
        serverName = String(advertisedDevice.getName().c_str());
      } else {
        serverName = "Unknown";
      }

      Serial.print("Target server found! Name: ");
      Serial.print(serverName);
      Serial.print(" | Address: ");
      Serial.println(advertisedDevice.getAddress().toString().c_str());

      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("===========================================");
  Serial.println("XIAO ESP32-C3 BLE Client Starting...");
  Serial.println("===========================================");

  // Initialize BLE device
  BLEDevice::init("XIAO_C3_CLIENT");

  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);

  Serial.println("Scanning for BLE servers...");
  Serial.println("Looking for service UUID:");
  Serial.println(serviceUUID.toString().c_str());
  Serial.println("===========================================");
  
  pBLEScan->start(5, false);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.print("Client successfully connected to ");
      Serial.println(serverName);
    } else {
      Serial.println("Failed to connect to the server.");
    }
    doConnect = false;
  }

  // If disconnected, restart scanning
  if (!connected && doScan) {
    Serial.println("Restarting scan...");
    BLEDevice::getScan()->start(0);
  }

  delay(1000);
}