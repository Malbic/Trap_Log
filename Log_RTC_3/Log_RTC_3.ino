#include <BluetoothSerial.h>
#include <FS.h>
#include <SPIFFS.h>
#include <RTClib.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>



// ====== GLOBAL OBJECTS ======
BluetoothSerial SerialBT;
RTC_DS3231 rtc;

// ====== CONFIG STRUCT ======
struct Config {
  String trapName = "Trap_Default";
  int lineCount = 30;
} config;

const char* configFilePath = "/config.json";
String logFilePath;
const int buttonPin = 12;
const unsigned long debounceDelay = 50;

// ====== BUTTON STATE ======
bool lastStableState = HIGH;
bool lastReadState = HIGH;
unsigned long lastDebounceTime = 0;

// ====== BLUETOOTH COMMAND BUFFER ======
String inputBuffer = "";

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  pinMode(buttonPin, INPUT_PULLUP);

  SerialBT.begin("TrapLogger");
  Serial.println("Bluetooth ready. Connect to: TrapLogger");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed!");
  }

  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
  } else if (rtc.lostPower()) {
    Serial.println("RTC lost power, needs sync.");
  }

  loadConfig();
  updateLogFilePath();

  // Sync system time with RTC on boot
  syncSystemTimeWithRTC();
}

// ====== MAIN LOOP ======
void loop() {
  checkBluetoothInput();
  checkButtonPress();
}

// ===== SYNC SYSTEM TIME FROM RTC =====
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
    SerialBT.println("READ_TIME - Show current RTC time");
    SerialBT.println("SET_NAME NewName");
    SerialBT.println("GET_NAME");
    SerialBT.println("SET_LINE_COUNT <number>");
    SerialBT.println("SHOW_CONFIG");
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
    if (dateTime.length() != 19) {
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

    // Delete old log file
    if (SPIFFS.exists(logFilePath)) {
      SPIFFS.remove(logFilePath);
    }

    config.trapName = newName;
    updateLogFilePath();
    saveConfig();

    SerialBT.println("Trap name updated to: " + config.trapName);
  }

  else if (cmd.equalsIgnoreCase("GET_NAME")) {
    SerialBT.println("Trap name: " + config.trapName);
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

  else if (cmd.equalsIgnoreCase("SHOW_CONFIG")) {
    SerialBT.println("Current Configuration:");
    SerialBT.println("Trap Name: " + config.trapName);
    SerialBT.println("Max Log Lines: " + String(config.lineCount));
  }

  else {
    SerialBT.println("Unknown command. Type HELP for list.");
  }
}

// ====== CONFIG FILE ======
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
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["trapName"] = config.trapName;
  doc["lineCount"] = config.lineCount;

  File file = SPIFFS.open(configFilePath, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open config file for writing.");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("Config saved.");
}

void updateLogFilePath() {
  logFilePath = "/" + config.trapName + ".log";
}

// ====== EVENT LOGGING ======
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
