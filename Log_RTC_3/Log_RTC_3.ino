#include <BluetoothSerial.h>
#include <FS.h>
#include <SPIFFS.h>
#include <RTClib.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>
#include <esp_wifi.h>  // <-- Add this for MAC address functions
#include <WiFi.h>


// ====== CONSTANTS ======
const int lisIntPin = 13; // LIS3DH INT1 connected to GPIO13
const int ledPin = 2;     // Onboard LED (typically GPIO2)
const int buttonPin = 12;
const unsigned long debounceDelay = 50;
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
  int tapCount;     // Add this line for tapCount
  int sensitivity;  // Add this line for sensitivity
} 
config;  // Declare an instance of the Config struct
const char* configFilePath = "/config.json";
String logFilePath;

// ====== BUTTON STATE ======
bool lastStableState = HIGH;
bool lastReadState = HIGH;
unsigned long lastDebounceTime = 0;

// ====== BLUETOOTH COMMAND BUFFER ======
String inputBuffer = "";

// ====== FUNCTION DECLARATIONS ======
void knockISR();
void syncSystemTimeWithRTC();
void checkButtonPress();
void checkBluetoothInput();
void processCommand(String cmd);
void loadConfig();
void saveConfig();
void updateLogFilePath();
void logEvent(String message);
void loadSettings();

String getMacAddress() {
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac); // <-- Corrected function and constant
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}



// ====== SETUP ======
void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA); // Initialize Wi-Fi hardware in Station mode
  WiFi.disconnect(true); // Disconnect from any network, but load real MAC address

  pinMode(lisIntPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);
  digitalWrite(ledPin, LOW);

  SerialBT.begin("TrapLogger");
  Serial.println("Bluetooth ready. Connect to: TrapLogger");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed!");
  }

  Wire.begin();
  if (!rtc.begin() || rtc.lostPower()) {
  Serial.println("RTC not available. Skipping system time sync.");
  return;
}
 // if (!rtc.begin()) {
 //   Serial.println("RTC not found!");
 // } else if (rtc.lostPower()) {
 //   Serial.println("RTC lost power, needs sync.");
 // }

  loadConfig();
  updateLogFilePath();
  syncSystemTimeWithRTC();

  if (!lis.begin(LIS3DH_I2C_ADDRESS)) { 
    Serial.println("Could not start LIS3DH!");
    while (1);
  }
  Serial.println("LIS3DH found!");
  lis.setRange(LIS3DH_RANGE_2_G); //2_G, 4_G, 8_G, 16_G
  loadSettings();
  lis.setClick(tapCount, sensitivity); // Single or Double tap detection, tap sensitivity

  attachInterrupt(digitalPinToInterrupt(lisIntPin), knockISR, FALLING);
Wire.beginTransmission(LIS3DH_I2C_ADDRESS); 
Wire.write(0x22);             // CTRL_REG3 address
Wire.write(0x80);             // Set CLICK interrupt on INT1
Wire.endTransmission();

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
    SerialBT.println("Knock detected! Event logged.");

    digitalWrite(ledPin, HIGH);
    delay(200);
    digitalWrite(ledPin, LOW);
  }

  checkButtonPress();
  checkBluetoothInput();
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

// ====== BUTTON HANDLING ======
void checkButtonPress() {
  bool currentRead = digitalRead(buttonPin);

  if (currentRead != lastReadState) {
    lastDebounceTime = millis();
    lastReadState = currentRead;
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (currentRead != lastStableState) {
      lastStableState = currentRead;
      if (currentRead == LOW) {
        Serial.println("Button pressed!");
        logEvent("TRIGGERED");
        SerialBT.println("Trap triggered! Event logged.");
      }
    }
  }
}

// ====== BLUETOOTH COMMAND HANDLING ======
void checkBluetoothInput() {
  while (SerialBT.available()) {
    char incoming = SerialBT.read();
    if (incoming == '\n') {
      processCommand(inputBuffer);
      inputBuffer = "";
    } else if (incoming != '\r') {
      inputBuffer += incoming;
    }
  }
}

void processCommand(String cmd) {
  cmd.trim();
  Serial.println("Command: " + cmd);

  if (cmd.equalsIgnoreCase("HELP")) {
    SerialBT.println("Commands:");
    SerialBT.println("SHOW_LOGS - Display log entries");
    SerialBT.println("CLEAR_LOGS - Clear all logs");
    SerialBT.println("SYNC_TIME - Sync system time from RTC");
    SerialBT.println("SET_RTC YYYY-MM-DD HH:MM:SS - Set RTC time");
    SerialBT.println("SET_NAME NewName");
    SerialBT.println("SET_LINE_COUNT <number>");
    SerialBT.println("ADD_NOTE <message> - Add a note to the logs");
    SerialBT.println("CURRENT_CONFIG - Show all settings");
    SerialBT.println("SET_TAP 1 or SET_TAP 2 - Single or Double Tap");
    SerialBT.println("SET_SENSITIVITY <1-127> - Set tap sensitivity");
  }

  else if (cmd.equalsIgnoreCase("SHOW_LOGS")) {
    File logFile = SPIFFS.open(logFilePath, FILE_READ);
    if (!logFile || logFile.size() == 0) {
      SerialBT.println("No logs found.");
    } else {
      while (logFile.available()) {
        SerialBT.write(logFile.read());
      }
    }
    logFile.close();
  }

  else if (cmd.equalsIgnoreCase("CLEAR_LOGS")) {
    SPIFFS.remove(logFilePath);
    SerialBT.println("Logs cleared.");
  }

  else if (cmd.equalsIgnoreCase("SYNC_TIME")) {
    syncSystemTimeWithRTC();
    SerialBT.println("System time synced with RTC.");
  }

  else if (cmd.startsWith("SET_RTC")) {
    String dateTime = cmd.substring(8);
    dateTime.trim();
    if (dateTime.length() < 19) {
      SerialBT.println("Invalid format. Use: SET_RTC YYYY-MM-DD HH:MM:SS");
      return;
    }
    // Now check if the characters in important places are correct
  if (dateTime.charAt(4) != '-' || dateTime.charAt(7) != '-' || dateTime.charAt(10) != ' ' ||
      dateTime.charAt(13) != ':' || dateTime.charAt(16) != ':') {
    SerialBT.println("Invalid format. Use: SET_RTC YYYY-MM-DD HH:MM:SS");
    return;
  }
    int year = dateTime.substring(0, 4).toInt();
    int month = dateTime.substring(5, 7).toInt();
    int day = dateTime.substring(8, 10).toInt();
    int hour = dateTime.substring(11, 13).toInt();
    int minute = dateTime.substring(14, 16).toInt();
    int second = dateTime.substring(17, 19).toInt();
    rtc.adjust(DateTime(year, month, day, hour, minute, second));
    SerialBT.println("RTC updated.");
    Serial.printf("RTC set to: %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);
  }

  else if (cmd.equalsIgnoreCase("READ_TIME")) {
    DateTime now = rtc.now();
    char rtcTime[30];
    sprintf(rtcTime, "%04d-%02d-%02d %02d:%02d:%02d",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute(), now.second());
    SerialBT.println("RTC Time: " + String(rtcTime));
  }

  else if (cmd.startsWith("SET_NAME")) {
    String newName = cmd.substring(8);
    newName.trim();
    if (newName.length() == 0) {
      SerialBT.println("Name cannot be empty.");
      return;
    }

    if (SPIFFS.exists(logFilePath)) {
      SPIFFS.remove(logFilePath);
    }

    config.trapName = newName;
    updateLogFilePath();
    saveConfig();

    SerialBT.println("Trap name updated to: " + config.trapName);
  }


  else if (cmd.startsWith("SET_LINE_COUNT")) {
    int newCount = cmd.substring(15).toInt();
    if (newCount <= 0) {
      SerialBT.println("Line count must be positive.");
      return;
    }
    config.lineCount = newCount;
    saveConfig();
    SerialBT.println("Line count set to: " + String(newCount));
  }
    
  else if (cmd.startsWith("ADD_NOTE")) {    // Add new "ADD_NOTE" command
    String noteMessage = cmd.substring(9);  // Remove "ADD_NOTE" part
    noteMessage.trim();

    if (noteMessage.length() == 0) {
      SerialBT.println("Please provide a message for the note.");
      return;
    }

    logEvent("NOTE " + noteMessage);  // Log the note with a timestamp
    SerialBT.println("Note added: " + noteMessage);
  }

else if (cmd.equalsIgnoreCase("CURRENT_CONFIG")) {
  SerialBT.println("===== Current Configuration =====");

  SerialBT.println("Trap Name: " + config.trapName);

  DateTime now = rtc.now();
  char rtcTime[30];
  sprintf(rtcTime, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  SerialBT.println("RTC Time: " + String(rtcTime));

  SerialBT.println("Max Log Lines: " + String(config.lineCount));


    SerialBT.println("MAC Address: " + getMacAddress());

  SerialBT.println("Tap Detection: " + String(config.tapCount == 1 ? "Single Tap" : "Double Tap"));
  SerialBT.println("Tap Sensitivity: " + String(config.sensitivity));
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
            SerialBT.println("Tap limit updated to " + String(tapCount));
        } else {
            Serial.println("Invalid tap limit. Must be 1 or 2.");
            SerialBT.println("Invalid tap limit. Must be 1 or 2.");
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
            SerialBT.println("Sensitivity updated to " + String(sensitivity));
        } else {
            Serial.println("Invalid sensitivity. Must be 1-127.");
            SerialBT.println("Invalid sensitivity. Must be 1-127.");
        }
    } else {
        Serial.println("No value provided for SET_SENSITIVITY");
    }
}



  else {
    SerialBT.println("Unknown command. Type HELP for list.");
  }
}
void handleCommand(String cmd) {
  if (cmd.startsWith("SET_TAP")) {      // Example: SET_TAP 1 or SET_TAP 2
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
  
  else if (cmd.startsWith("SET_SENSITIVITY")) {      // Example: SET_SENSITIVITY 100
    int newSensitivity = cmd.substring(16).toInt();  // Extract the value after "SET_SENSITIVITY "
    if (newSensitivity >= 1 && newSensitivity <= 127) {
      config.sensitivity = newSensitivity;  // Update the sensitivity value
      saveConfig();  // Save the updated config to SPIFFS
      Serial.print("Sensitivity set to: ");
      Serial.println(config.sensitivity);
    } else {
      Serial.println("Invalid sensitivity value. Use a value between 1 and 127.");
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
  File logFile = SPIFFS.open(logFilePath, FILE_APPEND);
  if (!logFile) {
    Serial.println("Failed to open log file");
    return;
  }

  DateTime now = rtc.now();
  char timeStr[30];
  sprintf(timeStr, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());

  logFile.printf("%s - %s - %s\n", config.trapName.c_str(), timeStr, message.c_str());
  logFile.close();
  Serial.printf("Logged: %s - %s\n", timeStr, message.c_str());

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
      if (newSensitivity >= 1 && newSensitivity <= 127) {
        sensitivity = newSensitivity;
        lis.setClick(tapCount, sensitivity);
        Serial.println("Sensitivity setting updated.");
      } else {
        Serial.println("Invalid sensitivity! Must be 1 to 127.");
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
      config.tapCount = doc["tapCount"] | 2; // default 2 if missing
      config.sensitivity = doc["sensitivity"] | 20; // default 20 if missing
    } else {
      Serial.println("Failed to parse config file.");
    }
    settingsFile.close();
  } else {
    Serial.println("Failed to open config file for reading.");
  }
}

