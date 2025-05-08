// ==========================================
// Filename: Log_RTC_3.ino
// Description: This program handles a trap logger using 
//              an ESP32. It manages wakeup, Bluetooth 
//              communication, RTC synchronization, and 
//              logging events to SPIFFS.
// Author: Malbic
// ==========================================

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
const unsigned long BLUETOOTH_TIMEOUT = 15000; // Bluetooth timeout in milliseconds
const unsigned long SERIAL_DELAY = 100;       // Delay for serial output in milliseconds

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
    printWakeupReason();
    if (isWakeupFromPin()) {
        if (isPinHeldLow()) {
            Serial.println("Pin LOW for >1 second. Enabling Bluetooth...");
            enableBluetooth();
        } else {
            Serial.println("Momentary pin LOW detected. Logging trap activation...");
            logEvent("TRAP ACTIVATION");
        }
    }
}

bool isWakeupFromPin() {
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
}

bool isPinHeldLow() {
    unsigned long startTime = millis();
    while (digitalRead(wakeupPin) == LOW) {
        if (millis() - startTime > bluetoothThreshold) {
            return true;
        }
    }
    return false;
}

// ====== ENABLE BLUETOOTH ======
void enableBluetooth() {
  SerialBT.begin("TrapLogger"); // Enable Bluetooth
  Serial.println("Bluetooth enabled. Waiting for connection...");

  // Wait for a client or timeout
  unsigned long bluetoothStart = millis();
  while (!SerialBT.hasClient() && (millis() - bluetoothStart < BLUETOOTH_TIMEOUT) {
    delay(100); // Check every 100ms
  }
    delay(SERIAL_DELAY); // Allow time for serial output
  if (SerialBT.hasClient()) {
    Serial.println("Bluetooth client connected.");
    handleBluetoothSession();
  } else {
    Serial.println("Bluetooth timeout. Returning to sleep...");
  }

  SerialBT.end(); // Disable Bluetooth
  enterDeepSleep();
}
void handleBluetoothSession() {
    Serial.println("Bluetooth session active.");
    while (SerialBT.available()) {
        String command = SerialBT.readStringUntil('\n');
        processBluetoothCommand(command);
    }
}

void processBluetoothCommand(String command) {
    if (command.startsWith("SET_TAP:")) {
        config.tapCount = command.substring(8).toInt();
        Serial.println("Tap count updated to: " + String(config.tapCount));
        saveConfig();
    } else if (command.startsWith("SET_SENS:")) {
        config.sensitivity = command.substring(9).toInt();
        Serial.println("Sensitivity updated to: " + String(config.sensitivity));
        saveConfig();
    } else {
        Serial.println("Unknown command: " + command);
    }
}
void logEvent(String message) {
    DateTime now = rtc.now();
    String timestamp = String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) +
                       " " + String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());
    String logMessage = timestamp + ": " + message;

    File logFile = SPIFFS.open(logFilePath, FILE_APPEND);
    if (logFile) {
        logFile.println(logMessage);
        logFile.close();
    }
    Serial.println(logMessage);
}
void logEvent(String message) {
    DateTime now = rtc.now();
    String timestamp = String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) +
                       " " + String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());
    String logMessage = timestamp + ": " + message;

    File logFile = SPIFFS.open(logFilePath, FILE_APPEND);
    if (logFile) {
        logFile.println(logMessage);
        logFile.close();
    }
    Serial.println(logMessage);
}
void updateLogFilePath() {
    DateTime now = rtc.now();
    logFilePath = "/log_" + String(now.year()) + "_" + String(now.month()) + "_" + String(now.day()) + ".txt";
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
