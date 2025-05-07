#include <BluetoothSerial.h>
#include <FS.h>
#include <SPIFFS.h>
#include <RTClib.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>
#include <esp_bt.h>
#include <esp_sleep.h>

// ====== CONSTANTS ======
const int wakeupPin = 12; // Single pin for wake-up (button or trap activation)
const int ledPin = 2;     // Onboard LED (typically GPIO2)
const unsigned long debounceDelay = 50;
const unsigned long bluetoothThreshold = 1000; // 1 second threshold for enabling Bluetooth
const uint8_t LIS3DH_I2C_ADDRESS = 0x19; // 0x18 or 0x19 is the LIS3DH I2C address

// ====== GLOBAL OBJECTS ======
BluetoothSerial SerialBT;
RTC_DS3231 rtc;
Adafruit_LIS3DH lis = Adafruit_LIS3DH();
int tapCount = 2;  // Default value, can be 1 or 2
int sensitivity = 20;    // default value, can be 1 to 127

// ====== FLAGS ======
volatile bool knockDetected = false;

// ====== CONFIG STRUCT ======
struct Config {
  String trapName = "Trap_Default";
  int lineCount = 50;
  int tapCount;     
  int sensitivity;  
} config; 
const char* configFilePath = "/config.json";
String logFilePath;

// ====== FUNCTION DECLARATIONS ======
void setup();
void loop();
void knockISR();
void handleWakeup();
void enterDeepSleep();
void enableBluetooth();
void syncSystemTimeWithRTC();
void logEvent(String message);
void loadConfig();
void saveConfig();
void updateLogFilePath();
void enforceLogLimit();
void printWakeupReason();
void handleBluetoothSession();
void checkBluetoothInput();
void loadSettings();

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(100); // Allow time for serial output

  // Configure the wake-up pin
  pinMode(wakeupPin, INPUT_PULLUP); // Configure pin with internal pull-up
  esp_sleep_enable_ext0_wakeup((gpio_num_t)wakeupPin, 0); // Wake up when pin goes LOW

  // Handle wake-up reason
  handleWakeup();

  // Initialize Bluetooth (disabled by default)
  SerialBT.begin("TrapLogger");
  Serial.println("Bluetooth ready. Connect to: TrapLogger");
  SerialBT.end(); // Disable Bluetooth unless explicitly enabled

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed!");
  }

  // Initialize RTC
  if (!rtc.begin() || rtc.lostPower()) {
    Serial.println("RTC not available. Skipping system time sync.");
  } else {
    syncSystemTimeWithRTC();
  }

  // Initialize LIS3DH
  if (!lis.begin(LIS3DH_I2C_ADDRESS)) {
    Serial.println("Could not start LIS3DH!");
    while (1);
  }
  Serial.println("LIS3DH found!");
  lis.setRange(LIS3DH_RANGE_2_G); // Set sensitivity range
  loadSettings();
  lis.setClick(config.tapCount, config.sensitivity); // Apply saved settings

  // Configure LIS3DH interrupt
  attachInterrupt(digitalPinToInterrupt(wakeupPin), knockISR, FALLING);

  // Update log file path
  updateLogFilePath();
}

// ====== MAIN LOOP ======
void loop() {
  // No repeated functionality in the main loop
  enterDeepSleep(); // Go back to deep sleep immediately
}

// ====== HANDLE WAKE-UP ======
void handleWakeup() {
  // Print the wake-up reason
  printWakeupReason();

  // Check if the wake-up was caused by the configured pin
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    unsigned long startTime = millis();

    // Measure how long the pin remains LOW
    while (digitalRead(wakeupPin) == LOW) {
      if (millis() - startTime > bluetoothThreshold) {
        // Pin is LOW for more than 1 second: Enable Bluetooth
        Serial.println("Pin LOW for >1 second. Enabling Bluetooth...");
        enableBluetooth();
        return;
      }
    }

    // Pin was LOW momentarily: Treat as trap activation
    Serial.println("Momentary pin LOW detected. Logging trap activation...");
    logEvent("TRAP ACTIVATION");
  }
}

// ====== ENABLE BLUETOOTH ======
void enableBluetooth() {
  SerialBT.begin("TrapLogger"); // Enable Bluetooth
  Serial.println("Bluetooth enabled. Waiting for connection...");

  // Wait for a client or timeout
  unsigned long bluetoothStart = millis();
  while (!SerialBT.hasClient() && (millis() - bluetoothStart < 15000)) {
    delay(100); // Check every 100ms
  }

  if (SerialBT.hasClient()) {
    Serial.println("Bluetooth client connected.");
    handleBluetoothSession();
  } else {
    Serial.println("Bluetooth timeout. Returning to sleep...");
  }

  SerialBT.end(); // Disable Bluetooth
  enterDeepSleep();
}

// ====== ENTER DEEP SLEEP ======
void enterDeepSleep() {
  Serial.println("Entering deep sleep...");
  delay(100); // Allow time for serial output
  esp_deep_sleep_start();
}

// ====== INTERRUPT SERVICE ROUTINE ======
void IRAM_ATTR knockISR() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime > debounceDelay) {
    knockDetected = true;
  }
  lastInterruptTime = interruptTime;
}

// ====== PRINT WAKE-UP REASON ======
void printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wakeup caused by external signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      break;
    default:
      Serial.println("Wakeup was not caused by deep sleep");
      break;
  }
}

