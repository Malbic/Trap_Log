// ==========================================
// Filename: Log_RTC_3.ino
// Description: This program handles a trap logger using 
//              an ESP32. It manages wakeup, Bluetooth 
//              communication, RTC synchronization, and 
//              logging events to SPIFFS.
// Author: Malbic
// ==========================================

#include <NimBLEDevice.h>
#include <FS.h>
#include <SPIFFS.h>
#include <RTClib.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>


#define UART_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_CHAR_RX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_CHAR_TX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

NimBLECharacteristic* pTxCharacteristic;

void processCommand(String cmd);

class UARTServerCallbacks : public NimBLECharacteristicCallbacks {
void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    std::string rxValue = pCharacteristic->getValue();
    if (!rxValue.empty()) {
      String cmd = String(rxValue.c_str());
      processCommand(cmd);  
    }
  }
};


// ====== CONSTANTS ======
const int lisIntPin = 3;  // LIS3DH INT1 connected to GPIO3
const int ledPin = 8;     // Onboard LED (typically GPIO8)
const unsigned long debounceDelay = 50;

// ====== GLOBAL OBJECTS ======

RTC_DS3231 rtc;
Adafruit_LIS3DH lis = Adafruit_LIS3DH();
int tapCount = 2;      // Default value, can be 1 or 2
int sensitivity = 20;  // default value, can be 1 to 200


// ====== FLAGS ======
volatile bool knockDetected = false;

// ====== CONFIG STRUCT ======
struct Config {
  String trapName = "Trap_Default";
  int lineCount = 30;
  int tapCount;     // Add this line for tapCount
  int sensitivity;  // Add this line for sensitivity
}  config;  // Declare an instance of the Config struct
const char* configFilePath = "/config.json";
String logFilePath;


// ====== BLUETOOTH COMMAND BUFFER ======
String inputBuffer = "";

// ====== FUNCTION DECLARATIONS ======
void knockISR();
void syncSystemTimeWithRTC();

void loadConfig();
void saveConfig();
void updateLogFilePath();
void logEvent(String message);
void loadSettings();


// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Debug code");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed!");
  }
  Serial.print("Total SPIFFS: ");
  Serial.println(SPIFFS.totalBytes());
  Serial.print("Used SPIFFS: ");
  Serial.println(SPIFFS.usedBytes());

  pinMode(lisIntPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  loadConfig();
  Serial.print("Loaded Trap Name: ");
  Serial.println(config.trapName);

  NimBLEDevice::init("TrapLogger");  // BLE device name
  Serial.println("Bluetooth ready. Connect to: TrapLogger");
  NimBLEServer* pServer = NimBLEDevice::createServer();
  NimBLEService* pService = pServer->createService(UART_SERVICE_UUID);

  NimBLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
    UART_CHAR_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRxCharacteristic->setCallbacks(new UARTServerCallbacks());

  pTxCharacteristic = pService->createCharacteristic(
    UART_CHAR_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY);

  pService->start();
  NimBLEDevice::startAdvertising();

  Serial.print("Total SPIFFS: ");
Serial.println(SPIFFS.totalBytes());
Serial.print("Used SPIFFS: ");
Serial.println(SPIFFS.usedBytes());

  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
  } else if (rtc.lostPower()) {
    Serial.println("RTC lost power, needs sync.");
  }
  Serial.println("RTC found!");

  loadConfig();
  Serial.print("Loaded Trap Name: ");
  Serial.println(config.trapName);
  updateLogFilePath();
  syncSystemTimeWithRTC();

  if (!lis.begin(0x19)) {  // 0x18 is the LIS3DH I2C address
    Serial.println("Could not start LIS3DH!");
    while (1);
  }
  Serial.println("LIS3DH found!");
  lis.setRange(LIS3DH_RANGE_2_G);
  loadSettings();
  lis.setClick(tapCount, sensitivity);  // Double tap detection

  attachInterrupt(digitalPinToInterrupt(lisIntPin), knockISR, FALLING);
  Wire.beginTransmission(0x19);  // Your sensor address (you said you changed it to 0x19)
  Wire.write(0x22);              // CTRL_REG3 address
  Wire.write(0x80);              // Set CLICK interrupt on INT1
  Wire.endTransmission();

  //float tempC = rtc.getTemperature();
  //Serial.print("Ambient Temp (DS3231): ");
  //Serial.print(tempC);
  //Serial.println(" °C");
}

// ====== INTERRUPT SERVICE ROUTINE ======
void IRAM_ATTR knockISR() {
  knockDetected = true;
}



// ====== MAIN LOOP ======


void loop() {
  if (knockDetected) {
    knockDetected = false;

    Serial.println("Knock detected!");
    logEvent("KNOCK DETECTED");

  if (pTxCharacteristic) { 
    pTxCharacteristic->setValue("Knock detected! Event logged.");
    pTxCharacteristic->notify();

    digitalWrite(ledPin, HIGH);
    delay(200);
    digitalWrite(ledPin, LOW);
    } 
  }
}

// ====== SYNC SYSTEM TIME FROM RTC ======
void syncSystemTimeWithRTC() {
  DateTime now = rtc.now();
  struct tm timeinfo;
  timeinfo.tm_year = now.year() - 1900;
  timeinfo.tm_mon  = now.month() - 1;
  timeinfo.tm_mday = now.day();
  timeinfo.tm_hour = now.hour();
  timeinfo.tm_min  = now.minute();
  timeinfo.tm_sec  = now.second();
  timeinfo.tm_isdst = 0;

  time_t t = mktime(&timeinfo);
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);

  Serial.println("System time synced with RTC.");
}

void processCommand(String cmd) {
  cmd.trim();
  Serial.println("Command: " + cmd);

  if (cmd.equalsIgnoreCase("HELP")) {
    pTxCharacteristic->setValue("Commands:");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("SHOW_LOGS - Display log entries");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("CLEAR_LOGS - Clear all logs");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("SYNC_TIME - Sync system time from RTC");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("SET_RTC YYYY-MM-DD HH:MM:SS - Set RTC time");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("SET_NAME NewName");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("SET_LINE_COUNT <number>");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("ADD_NOTE <message> - Add a note to the logs");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("CURRENT_CONFIG - Show all settings");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("SET_TAP 1 or SET_TAP 2 - Single or Double Tap Detection");
pTxCharacteristic->notify();
    pTxCharacteristic->setValue("SET_SENSITIVITY <1-127> - Set tap sensitivity");
pTxCharacteristic->notify();
 //   pTxCharacteristic->setValue("READ_TEMP - Show internal ESP32 temperature");
//pTxCharacteristic->notify();

  }

else if (cmd.equalsIgnoreCase("SHOW_LOGS")) {
    File logFile = SPIFFS.open(logFilePath, FILE_READ);
    if (!logFile || logFile.size() == 0) {
        pTxCharacteristic->setValue("No logs found.");
        pTxCharacteristic->notify();
    } else {
        String bleBuffer = "";
        while (logFile.available()) {
            char c = logFile.read();
            bleBuffer += c;
            // BLE notification size limit (20 bytes is safe for most clients)
            if (bleBuffer.length() >= 20) {
                pTxCharacteristic->setValue(bleBuffer.c_str());
                pTxCharacteristic->notify();
                bleBuffer = "";
                delay(10); // Give BLE stack time to transmit
            }
        }
        // Send any remaining data
        if (bleBuffer.length() > 0) {
            pTxCharacteristic->setValue(bleBuffer.c_str());
            pTxCharacteristic->notify();
        }
    }
    logFile.close();
}

  else if (cmd.equalsIgnoreCase("CLEAR_LOGS")) {
    SPIFFS.remove(logFilePath);
    pTxCharacteristic->setValue("Logs cleared.");
pTxCharacteristic->notify();
  }

  else if (cmd.equalsIgnoreCase("SYNC_TIME")) {
    syncSystemTimeWithRTC();
    pTxCharacteristic->setValue("System time synced with RTC.");
pTxCharacteristic->notify();
  }

  else if (cmd.startsWith("SET_RTC")) {
    String dateTime = cmd.substring(8);
    dateTime.trim();
    if (dateTime.length() < 19) {
      pTxCharacteristic->setValue("Invalid format. Use: SET_RTC YYYY-MM-DD HH:MM:SS");
pTxCharacteristic->notify();
      return;
    }
    // Now check if the characters in important places are correct
  if (dateTime.charAt(4) != '-' || dateTime.charAt(7) != '-' || dateTime.charAt(10) != ' ' ||
      dateTime.charAt(13) != ':' || dateTime.charAt(16) != ':') {
    pTxCharacteristic->setValue("Invalid format. Use: SET_RTC YYYY-MM-DD HH:MM:SS");
pTxCharacteristic->notify();
    return;
  }
    int year = dateTime.substring(0, 4).toInt();
    int month = dateTime.substring(5, 7).toInt();
    int day = dateTime.substring(8, 10).toInt();
    int hour = dateTime.substring(11, 13).toInt();
    int minute = dateTime.substring(14, 16).toInt();
    int second = dateTime.substring(17, 19).toInt();
    rtc.adjust(DateTime(year, month, day, hour, minute, second));
    pTxCharacteristic->setValue("RTC updated.");
pTxCharacteristic->notify();
    Serial.printf("RTC set to: %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);
  }

  else if (cmd.equalsIgnoreCase("READ_TIME")) {
    DateTime now = rtc.now();
    char rtcTime[30];
    sprintf(rtcTime, "%04d-%02d-%02d %02d:%02d:%02d",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute(), now.second());
    pTxCharacteristic->setValue("RTC Time: " + String(rtcTime));
pTxCharacteristic->notify();
  }

  else if (cmd.startsWith("SET_NAME")) {
    String newName = cmd.substring(8);
    newName.trim();
    if (newName.length() == 0) {
      pTxCharacteristic->setValue("Name cannot be empty.");
pTxCharacteristic->notify();
      return;
    }

    if (SPIFFS.exists(logFilePath)) {
      SPIFFS.remove(logFilePath);
    }

    config.trapName = newName;
    updateLogFilePath();
    saveConfig();

    pTxCharacteristic->setValue("Trap name updated to: " + config.trapName);
pTxCharacteristic->notify();
  }


  else if (cmd.startsWith("SET_LINE_COUNT")) {
    int newCount = cmd.substring(15).toInt();
    if (newCount <= 0) {
      pTxCharacteristic->setValue("Line count must be positive.");
pTxCharacteristic->notify();
      return;
    }
    config.lineCount = newCount;
    saveConfig();
    pTxCharacteristic->setValue("Line count set to: " + String(newCount));
pTxCharacteristic->notify();
  }
    
  else if (cmd.startsWith("ADD_NOTE")) {    // Add new "ADD_NOTE" command
    String noteMessage = cmd.substring(9);  // Remove "ADD_NOTE" part
    noteMessage.trim();

    if (noteMessage.length() == 0) {
      pTxCharacteristic->setValue("Please provide a message for the note.");
pTxCharacteristic->notify();
      return;
    }

    logEvent("NOTE " + noteMessage);  // Log the note with a timestamp
    pTxCharacteristic->setValue("Note added: " + noteMessage);
pTxCharacteristic->notify();
  }

else if (cmd.equalsIgnoreCase("CURRENT_CONFIG")) {
  pTxCharacteristic->setValue("your string\n");
  pTxCharacteristic->notify();("===== Current Configuration =====");

  pTxCharacteristic->setValue("Trap Name: " + config.trapName);
  pTxCharacteristic->notify();

  DateTime now = rtc.now();
  char rtcTime[30];
  sprintf(rtcTime, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  pTxCharacteristic->setValue("RTC Time: " + String(rtcTime));
  pTxCharacteristic->notify();

      // Only call getTemperature() here, safely
    float tempC = rtc.getTemperature();
    char tempMsg[40];
    snprintf(tempMsg, sizeof(tempMsg), "DS3231 Temp: %.2f C", tempC);
    pTxCharacteristic->setValue(tempMsg);
    pTxCharacteristic->notify();

  pTxCharacteristic->setValue("Max Log Lines: " + String(config.lineCount));
  pTxCharacteristic->notify();


  pTxCharacteristic->setValue("Tap Detection: " + String(config.tapCount == 1 ? "Single Tap" : "Double Tap"));
  pTxCharacteristic->notify();
  pTxCharacteristic->setValue("Tap Sensitivity: " + String(config.sensitivity));
pTxCharacteristic->notify();
}


else if (cmd.startsWith("SET_TAP")) {
    int spaceIndex = cmd.indexOf(' ');
    if (spaceIndex != -1) {
        String valueStr = cmd.substring(spaceIndex + 1);
        int newValue = valueStr.toInt();
        if (newValue == 1 || newValue == 2) {
            tapCount = newValue;
            config.tapCount = tapCount;
            lis.setClick(tapCount, sensitivity);  // re-apply settings to sensor
            saveConfig();
            Serial.println("Tap limit updated to " + String(tapCount));
            pTxCharacteristic->setValue("Tap limit updated to " + String(tapCount));
pTxCharacteristic->notify();
        } else {
            Serial.println("Invalid tap limit. Must be 1 or 2.");
            pTxCharacteristic->setValue("Invalid tap limit. Must be 1 or 2.");
pTxCharacteristic->notify();
        }
    } else {
        Serial.println("No value provided for SET_TAP");
    }
}

else if (cmd.startsWith("SET_SENSITIVITY")) {
    int spaceIndex = cmd.indexOf(' ');
    if (spaceIndex != -1) {
        String valueStr = cmd.substring(spaceIndex + 1);
        int newValue = valueStr.toInt();
        if (newValue >= 1 && newValue <= 127) {
            sensitivity = newValue;
            config.sensitivity = sensitivity;
            lis.setClick(tapCount, sensitivity);  // re-apply settings to sensor
            saveConfig();
            Serial.println("Sensitivity updated to " + String(sensitivity));
            pTxCharacteristic->setValue("Sensitivity updated to " + String(sensitivity));
pTxCharacteristic->notify();
        } else {
            Serial.println("Invalid sensitivity. Must be 1-127.");
            pTxCharacteristic->setValue("Invalid sensitivity. Must be 1-127.");
pTxCharacteristic->notify();
        }
    } else {
        Serial.println("No value provided for SET_SENSITIVITY");
    }
}

  else {
    pTxCharacteristic->setValue("Unknown command. Type HELP for list.");
pTxCharacteristic->notify();
  }

 

}
void handleCommand(String cmd) {
  if (cmd.startsWith("SET_TAP")) {
    // Example: SET_TAP 1 or SET_TAP 2
    int newtapCount = cmd.substring(8).toInt(); // Extract the number after "SET_TAP "
    if (newtapCount == 1 || newtapCount == 2) {
      config.tapCount = newtapCount;  // Update the tap limit
      saveConfig();  // Save the updated config to SPIFFS
      Serial.print("Tap limit set to: ");
      Serial.println(config.tapCount);
    } else {
      Serial.println("Invalid tap limit value. Use 1 or 2.");
    }
  }
  
  else if (cmd.startsWith("SET_SENSITIVITY")) {
    // Example: SET_SENSITIVITY 100
    int newSensitivity = cmd.substring(16).toInt();  // Extract the value after "SET_SENSITIVITY "
    if (newSensitivity >= 1 && newSensitivity <= 127) {
      config.sensitivity = newSensitivity;  // Update the sensitivity value
      saveConfig();  // Save the updated config to SPIFFS
      Serial.print("Sensitivity set to: ");
      Serial.println(config.sensitivity);
    } else {
      Serial.println("Invalid sensitivity value. Use a value between 1 and 200.");
    }
  }
}
// ====== CONFIG FILE HANDLING ======
void loadConfig() {
  File file = SPIFFS.open(configFilePath, FILE_READ);
  if (!file) {
    Serial.println("No config file found. Using default.");
    saveConfig();
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.println("Failed to parse config. Using default.");
    saveConfig();
    return;
  }

  config.trapName = doc["trapName"].as<String>();
  config.lineCount = doc["lineCount"] | 30;
  config.tapCount = doc["tapCount"] | 2;           // Set default value for tapCount
  config.sensitivity = doc["sensitivity"] | 20;   // Set default value for sensitivity
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["trapName"] = config.trapName;
  doc["lineCount"] = config.lineCount;
  doc["tapCount"] = config.tapCount;
  doc["sensitivity"] = config.sensitivity;

  File file = SPIFFS.open(configFilePath, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open config file for writing!");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("Configuration saved.");
}

void updateLogFilePath() {
  logFilePath = "/" + config.trapName + "_logs.txt";
}

 // ====== LOGGING FUNCTION ======
void logEvent(String message) {
    Serial.println("logEvent called with msg: " + message);


  File logFile = SPIFFS.open(logFilePath, FILE_APPEND);
  if (!logFile) {
    Serial.println("Failed to open log file");
    return;
  }
/*
  DateTime now = rtc.now();
  float tempC = rtc.getTemperature();  // Read DS3231 temperature

  char timeStr[30];
  sprintf(timeStr, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());   // Format: TrapName - Timestamp - Message - Temp

  logFile.printf("%s - %s - %s - Temp: %.2f C\n", config.trapName.c_str(), timeStr, message.c_str(), tempC);
  logFile.close();
  Serial.printf("Logged: %s - %s - Temp: %.2f C\n", message.c_str(), tempC);
*/
  // Enforce log limit
  enforceLogLimit();
  
}

// ====== LOG LIMIT ENFORCEMENT ======
void enforceLogLimit() {
  File logFile = SPIFFS.open(logFilePath, FILE_READ);
  if (!logFile) return;

  std::vector<String> lines;
  while (logFile.available()) {
    lines.push_back(logFile.readStringUntil('\n'));
  }
  logFile.close();

  if (lines.size() > config.lineCount) {
    lines.erase(lines.begin(), lines.begin() + (lines.size() - config.lineCount));

    File newFile = SPIFFS.open(logFilePath, FILE_WRITE);
    for (String line : lines) {
      newFile.println(line);
    }
    newFile.close();
    Serial.println("Old logs trimmed to maintain line limit.");
  }
}
void handleBluetoothCommands() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim(); // Remove any whitespace/newline

    if (input.startsWith("SET_TAP ")) {
      int newTap = input.substring(8).toInt();
      if (newTap == 1 || newTap == 2) {
        tapCount = newTap;
        lis.setClick(tapCount, sensitivity);
        Serial.println("Tap setting updated.");
      } else {
        Serial.println("Invalid tap setting! Must be 1 or 2.");
      }
    } 
    else if (input.startsWith("SET_SENSITIVITY ")) {
      int newSensitivity = input.substring(16).toInt();
      if (newSensitivity >= 1 && newSensitivity <= 200) {
        sensitivity = newSensitivity;
        lis.setClick(tapCount, sensitivity);
        Serial.println("Sensitivity setting updated.");
      } else {
        Serial.println("Invalid sensitivity! Must be 1 to 200.");
      }
    } 
    else {
      Serial.println("Unknown command.");
    }
  }
}
void loadSettings() {
  File settingsFile = SPIFFS.open(configFilePath, "r");
  if (settingsFile) {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, settingsFile);
    if (!error) {
      config.tapCount = doc["tapCount"] | 0; // default 0 if missing
      config.sensitivity = doc["sensitivity"] | 127; // default 127 if missing
    } else {
      Serial.println("Failed to parse config file.");
    }
    settingsFile.close();
  } else {
    Serial.println("Failed to open config file for reading.");
  }
}


