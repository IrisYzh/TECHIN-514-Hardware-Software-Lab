#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include "secrets.h"

// ============================================
// POWER MANAGEMENT CONFIGURATION
// ============================================

// Sleep Durations (milliseconds)
const uint32_t DEEP_SLEEP_NORMAL_MS = 10000;      // 10 seconds - normal monitoring
const uint32_t DEEP_SLEEP_EXTENDED_MS = 30000;    // 30 seconds - quiet period
const uint32_t QUICK_CHECK_DURATION_MS = 500;     // 0.5 seconds - quick sensor read

// Active Monitoring
const uint32_t ACTIVE_MONITOR_DURATION_MS = 30000;  // 30 seconds - active monitoring
const uint32_t ACTIVE_MONITOR_INTERVAL_MS = 2000;   // 2 seconds - check interval when active

// Motion Detection
const float MOTION_THRESHOLD_CM = 10.0;           // 10 cm change = motion detected
const uint32_t MOTION_CONFIRM_TIME_MS = 2000;     // 2 seconds - confirm motion is real
const uint32_t BASELINE_UPDATE_INTERVAL_MS = 300000; // 5 minutes - update baseline

// Upload Control
const uint32_t MIN_UPLOAD_INTERVAL_MS = 60000;    // 60 seconds - minimum between uploads
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 5000;    // 5 seconds - WiFi connection timeout
const uint32_t UPLOAD_TIMEOUT_MS = 3000;          // 3 seconds - Firebase upload timeout

// Adaptive Behavior
const uint32_t QUIET_PERIOD_THRESHOLD_MS = 300000;  // 5 minutes - no motion = quiet
const uint8_t HIGH_ACTIVITY_THRESHOLD = 5;          // 5 events/minute = high activity

// Sensor Configuration
const int PIN_TRIG = 2;  // D0 on XIAO ESP32-C3
const int PIN_ECHO = 3;  // D1 on XIAO ESP32-C3

// ============================================
// RTC MEMORY (Persists Through Deep Sleep)
// ============================================

typedef enum {
  STATE_DEEP_SLEEP,
  STATE_QUICK_CHECK,
  STATE_ACTIVE_MONITOR,
  STATE_UPLOAD_EVENT
} DeviceState;

RTC_DATA_ATTR DeviceState g_state = STATE_QUICK_CHECK;
RTC_DATA_ATTR float g_baseline_distance = -1.0;
RTC_DATA_ATTR uint32_t g_last_motion_time = 0;
RTC_DATA_ATTR uint32_t g_last_upload_time = 0;
RTC_DATA_ATTR uint32_t g_last_baseline_update = 0;
RTC_DATA_ATTR uint32_t g_motion_event_count = 0;
RTC_DATA_ATTR uint32_t g_total_uploads = 0;
RTC_DATA_ATTR uint32_t g_boot_count = 0;
RTC_DATA_ATTR bool g_motion_active = false;

// ============================================
// FIREBASE OBJECTS
// ============================================

UserAuth user_auth(FIREBASE_API_KEY, FIREBASE_USER_EMAIL, FIREBASE_USER_PASSWORD);
FirebaseApp app;
WiFiClientSecure ssl_client1, ssl_client2;
using AsyncClient = AsyncClientClass;
AsyncClient async_client1(ssl_client1), async_client2(ssl_client2);
RealtimeDatabase Database;
AsyncResult dbResult;

bool firebaseReady = false;

// ============================================
// HELPER FUNCTIONS
// ============================================

void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;
  
  if (aResult.isError()) {
    Serial.printf("Firebase Error: %s\n", aResult.error().message().c_str());
  }
  
  if (aResult.available()) {
    Serial.printf("Firebase Success: %s\n", aResult.uid().c_str());
  }
}

float readUltrasonicDistance() {
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  uint32_t duration = pulseIn(PIN_ECHO, HIGH, 30000);
  
  if (duration == 0) {
    return -1.0;
  }

  float distanceCm = duration / 58.2;
  
  // Validate range
  if (distanceCm < 2.0 || distanceCm > 400.0) {
    return -1.0;
  }
  
  return distanceCm;
}

bool connectWiFi() {
  Serial.println("WiFi: Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t startTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi: Connected!");
    return true;
  } else {
    Serial.println("\nWiFi: Failed!");
    return false;
  }
}

void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi: Disconnected");
}

bool initFirebase() {
  Serial.println("Firebase: Initializing...");
  
  ssl_client1.setInsecure();
  ssl_client2.setInsecure();
  
  initializeApp(async_client1, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(FIREBASE_RTDB_URL);
  
  uint32_t startTime = millis();
  while (!app.ready() && (millis() - startTime) < UPLOAD_TIMEOUT_MS) {
    app.loop();
    delay(50);
  }
  
  if (app.ready()) {
    Serial.println("Firebase: Ready!");
    return true;
  } else {
    Serial.println("Firebase: Timeout!");
    return false;
  }
}

void enterDeepSleep(uint32_t durationMs) {
  Serial.printf("Entering deep sleep for %u seconds\n", durationMs / 1000);
  Serial.flush();
  
  esp_sleep_enable_timer_wakeup((uint64_t)durationMs * 1000ULL);
  esp_deep_sleep_start();
}

void updateBaseline(float distance) {
  if (distance > 0) {
    g_baseline_distance = distance;
    g_last_baseline_update = millis();
    Serial.printf("Baseline updated: %.2f cm\n", g_baseline_distance);
  }
}

bool detectMotion(float currentDistance) {
  if (g_baseline_distance < 0 || currentDistance < 0) {
    return false;
  }
  
  float change = abs(currentDistance - g_baseline_distance);
  return (change > MOTION_THRESHOLD_CM);
}

// ============================================
// STATE: QUICK CHECK
// ============================================

void stateQuickCheck() {
  Serial.println("\n=== STATE: QUICK CHECK ===");
  Serial.printf("Boot #%u | Uptime: %u ms\n", g_boot_count, millis());
  
  // Read sensor
  float distance = readUltrasonicDistance();
  
  if (distance < 0) {
    Serial.println("Sensor read failed, returning to sleep");
    g_state = STATE_DEEP_SLEEP;
    return;
  }
  
  Serial.printf("Distance: %.2f cm | Baseline: %.2f cm\n", distance, g_baseline_distance);
  
  // Initialize baseline on first boot
  if (g_baseline_distance < 0) {
    updateBaseline(distance);
    g_state = STATE_DEEP_SLEEP;
    return;
  }
  
  // Update baseline periodically if stable
  if ((millis() - g_last_baseline_update) > BASELINE_UPDATE_INTERVAL_MS && !g_motion_active) {
    updateBaseline(distance);
  }
  
  // Check for motion
  if (detectMotion(distance)) {
    Serial.println(">>> MOTION DETECTED! <<<");
    g_motion_active = true;
    g_last_motion_time = millis();
    g_motion_event_count++;
    g_state = STATE_ACTIVE_MONITOR;
  } else {
    Serial.println("No motion detected");
    
    // Check if quiet period (no motion for 5+ minutes)
    if (g_last_motion_time > 0 && (millis() - g_last_motion_time) > QUIET_PERIOD_THRESHOLD_MS) {
      Serial.println("Quiet period detected - entering extended sleep");
      enterDeepSleep(DEEP_SLEEP_EXTENDED_MS);
    } else {
      enterDeepSleep(DEEP_SLEEP_NORMAL_MS);
    }
  }
}

// ============================================
// STATE: ACTIVE MONITOR
// ============================================

void stateActiveMonitor() {
  Serial.println("\n=== STATE: ACTIVE MONITOR ===");
  Serial.println("Monitoring for 30 seconds with 2-second intervals");
  
  uint32_t startTime = millis();
  float lastDistance = -1.0;
  bool motionConfirmed = false;
  uint32_t stableMotionStart = 0;
  
  while (millis() - startTime < ACTIVE_MONITOR_DURATION_MS) {
    float distance = readUltrasonicDistance();
    
    if (distance > 0) {
      Serial.printf("[%.1fs] Distance: %.2f cm", (millis() - startTime) / 1000.0, distance);
      
      bool motion = detectMotion(distance);
      
      if (motion) {
        Serial.println(" - MOTION");
        
        // Check if motion is stable (same for 2+ seconds)
        if (lastDistance > 0 && abs(distance - lastDistance) < 5.0) {
          if (stableMotionStart == 0) {
            stableMotionStart = millis();
          } else if (millis() - stableMotionStart >= MOTION_CONFIRM_TIME_MS) {
            if (!motionConfirmed) {
              motionConfirmed = true;
              Serial.println(">>> MOTION CONFIRMED! <<<");
            }
          }
        } else {
          stableMotionStart = 0;
        }
      } else {
        Serial.println(" - No motion");
        stableMotionStart = 0;
      }
      
      lastDistance = distance;
    } else {
      Serial.println("Sensor read failed");
    }
    
    delay(ACTIVE_MONITOR_INTERVAL_MS);
  }
  
  // Decision: Upload or return to sleep
  if (motionConfirmed) {
    Serial.println("Motion event confirmed - proceeding to upload");
    
    // Check if enough time has passed since last upload
    if ((millis() - g_last_upload_time) >= MIN_UPLOAD_INTERVAL_MS) {
      g_state = STATE_UPLOAD_EVENT;
    } else {
      Serial.println("Upload rate limit - skipping upload");
      g_motion_active = false;
      enterDeepSleep(DEEP_SLEEP_NORMAL_MS);
    }
  } else {
    Serial.println("Motion not confirmed - false alarm");
    g_motion_active = false;
    enterDeepSleep(DEEP_SLEEP_NORMAL_MS);
  }
}

// ============================================
// STATE: UPLOAD EVENT
// ============================================

void stateUploadEvent() {
  Serial.println("\n=== STATE: UPLOAD EVENT ===");
  
  uint32_t uploadStartTime = millis();
  
  // Connect WiFi
  if (!connectWiFi()) {
    Serial.println("WiFi connection failed - aborting upload");
    g_motion_active = false;
    enterDeepSleep(DEEP_SLEEP_NORMAL_MS);
    return;
  }
  
  // Initialize Firebase
  if (!initFirebase()) {
    Serial.println("Firebase initialization failed - aborting upload");
    disconnectWiFi();
    g_motion_active = false;
    enterDeepSleep(DEEP_SLEEP_NORMAL_MS);
    return;
  }
  
  // Read final distance
  float distance = readUltrasonicDistance();
  uint32_t timestamp = millis();
  
  // Upload event data
  Serial.println("Uploading motion event to Firebase...");
  
  String eventPath = "/motion_detection/events/event_" + String(g_total_uploads);
  
  Database.set<float>(async_client1, eventPath + "/distance_cm", distance, processData, "upload_distance");
  Database.set<uint32_t>(async_client1, eventPath + "/timestamp_ms", timestamp, dbResult);
  Database.set<uint32_t>(async_client1, eventPath + "/boot_count", g_boot_count, dbResult);
  Database.set<bool>(async_client1, eventPath + "/motion_detected", true, dbResult);
  
  // Update statistics
  Database.set<uint32_t>(async_client1, "/motion_detection/stats/total_events", g_total_uploads + 1, dbResult);
  Database.set<uint32_t>(async_client1, "/motion_detection/stats/last_event_time", timestamp, dbResult);
  Database.set<float>(async_client1, "/motion_detection/stats/last_distance", distance, dbResult);
  
  // Wait for upload to complete
  uint32_t waitStart = millis();
  while (millis() - waitStart < UPLOAD_TIMEOUT_MS) {
    app.loop();
    processData(dbResult);
    delay(50);
  }
  
  uint32_t uploadDuration = millis() - uploadStartTime;
  Serial.printf("Upload complete in %u ms\n", uploadDuration);
  
  // Disconnect WiFi immediately
  disconnectWiFi();
  
  // Update counters
  g_total_uploads++;
  g_last_upload_time = millis();
  g_motion_active = false;
  
  // Print statistics
  Serial.println("\n--- Statistics ---");
  Serial.printf("Total Uploads: %u\n", g_total_uploads);
  Serial.printf("Motion Events: %u\n", g_motion_event_count);
  Serial.printf("Boot Count: %u\n", g_boot_count);
  
  // Return to deep sleep
  enterDeepSleep(DEEP_SLEEP_NORMAL_MS);
}

// ============================================
// ARDUINO SETUP
// ============================================

void setup() {
  Serial.begin(115200);
  delay(500);
  
  g_boot_count++;
  
  Serial.println("\n\n");
  Serial.println("==========================================");
  Serial.println("  Smart Motion Detection System");
  Serial.println("  24-Hour Battery Operation");
  Serial.println("==========================================");
  Serial.printf("Boot #%u\n", g_boot_count);
  Serial.printf("Total Uploads: %u\n", g_total_uploads);
  Serial.printf("Motion Events: %u\n", g_motion_event_count);
  
  // Display wake reason
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  Serial.print("Wake Reason: ");
  
  switch (wakeReason) {
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Timer (from Deep Sleep)");
      break;
    default:
      Serial.println("Power On / Reset");
      g_state = STATE_QUICK_CHECK;
      break;
  }
  
  Serial.println("==========================================\n");
}

// ============================================
// ARDUINO LOOP
// ============================================

void loop() {
  switch (g_state) {
    case STATE_QUICK_CHECK:
      stateQuickCheck();
      break;
      
    case STATE_ACTIVE_MONITOR:
      stateActiveMonitor();
      break;
      
    case STATE_UPLOAD_EVENT:
      stateUploadEvent();
      break;
      
    default:
      Serial.println("Unknown state - resetting to QUICK_CHECK");
      g_state = STATE_QUICK_CHECK;
      break;
  }
  
  // This point is only reached if not entering deep sleep
  // Should not normally happen
  delay(1000);
}