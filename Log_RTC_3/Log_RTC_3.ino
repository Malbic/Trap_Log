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
#include <WiFi.h>
#include <WebServer.h>

// ====== Wi-Fi Access Point Setup ======
const char* ssid = "TrapLogger";
const char* password = "trap1234"; // Simple field password
WebServer server(80);

// ====== BLE Definitions ======
#define UART_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_CHAR_RX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_CHAR_TX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define KNOCK_IGNORE_TIME 2000
#define BLE_CHUNK_SIZE 20
unsigned long knockIgnoreUntil = 0;

NimBLECharacteristic* pTxCharacteristic;

// ====== Function Declarations ======
void processCommand(String cmd);
void handleRoot();
void handleShowLogs();
void handleClearLogs();
void knockISR();
void syncSystemTimeWithRTC();
void loadConfig();
void saveConfig();
void updateLogFilePath();
void logEvent(String message);
void loadSettings();

// ====== GLOBALS ======
const int lisIntPin = 3;  // LIS3DH INT1 connected to GPIO3
const int ledPin = 8;     // Onboard LED (typically GPIO8)
const unsigned long debounceDelay = 50;
File logFile;
int logLinesSent = 0;
int linesPerPage = 10;

RTC_DS3231 rtc;
Adafruit_LIS3DH lis = Adafruit_LIS3DH();
int tapCount = 2;      // Default value
int sensitivity = 20;  // Default value

volatile bool knockDetected = false;

struct Config {
  String trapName = "Trap_Default";
  int lineCount = 30;
  int tapCount;
  int sensitivity;
} config;
const char* configFilePath = "/config.json";
String logFilePath;

String inputBuffer = "";

// ====== BLE UART Callbacks ======
class UARTServerCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    std::string rxValue = pCharacteristic->getValue();
    if (!rxValue.empty()) {
      String cmd = String(rxValue.c_str());
      processCommand(cmd);
    }
  }
};

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Trap Logger Starting...");
  knockIgnoreUntil = millis() + KNOCK_IGNORE_TIME;

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed!");
  }

  pinMode(lisIntPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  // === Wi-Fi AP Mode ===
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // --- Web Server Endpoints ---
  server.on("/", HTTP_GET, handleRoot);
  server.on("/show_logs", HTTP_GET, handleShowLogs);
  server.on("/clear_logs", HTTP_POST, handleClearLogs);
  server.begin();
  Serial.println("HTTP server started at /");

  // ===== BLE SETUP =====
  NimBLEDevice::init("TrapLogger");
  Serial.println("Bluetooth ready. Connect to: TrapLogger");
  NimBLEServer* pServer = NimBLEDevice::createServer();
  NimBLEService* pService = pServer->createService(UART_SERVICE_UUID);

  NimBLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
    UART_CHAR_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRxCharacteristic->setCallbacks(new UARTServerCallbacks());

  pTxCharacteristic = pService->createCharacteristic(
    UART_CHAR_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);

  pService->start();
  NimBLEDevice::startAdvertising();

  Serial.print("Total SPIFFS: "); Serial.println(SPIFFS.totalBytes());
  Serial.print("Used SPIFFS: "); Serial.println(SPIFFS.usedBytes());

  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
  } else if (rtc.lostPower()) {
    Serial.println("RTC lost power, needs sync.");
  }
  Serial.println("RTC found!");

  loadConfig();
  Serial.print("Loaded Trap Name: "); Serial.println(config.trapName);
  updateLogFilePath();
  syncSystemTimeWithRTC();

  if (!lis.begin(0x19)) {
    Serial.println("Could not start LIS3DH!");
    while (1);
  }
  Serial.println("LIS3DH found!");
  lis.setRange(LIS3DH_RANGE_2_G);
  loadSettings();
  lis.setClick(tapCount, sensitivity);

  attachInterrupt(digitalPinToInterrupt(lisIntPin), knockISR, FALLING);
  Wire.beginTransmission(0x19);
  Wire.write(0x22);
  Wire.write(0x80);
  Wire.endTransmission();

  float tempC = rtc.getTemperature();
  Serial.print("Ambient Temp (DS3231): "); Serial.print(tempC); Serial.println(" °C");
}

// ====== MAIN LOOP ======
void loop() {
  server.handleClient(); // Wi-Fi HTTP server
  if (knockDetected) {
    knockDetected = false;
    logEvent("KNOCK DETECTED");
    if (pTxCharacteristic) {
      DateTime now = rtc.now();
      float tempC = rtc.getTemperature();
      char bleMsg[64];
      int msgLen = snprintf(bleMsg, sizeof(bleMsg),
        "Knock! %04d-%02d-%02d %02d:%02d:%02d Temp: %.1f C\r\n",
        now.year(), now.month(), now.day(),
        now.hour(), now.minute(), now.second(), tempC);
      bleMsg[sizeof(bleMsg) - 1] = '\0';
      pTxCharacteristic->setValue((uint8_t*)bleMsg, msgLen);
      pTxCharacteristic->notify();
      digitalWrite(ledPin, HIGH);
      delay(200);
      digitalWrite(ledPin, LOW);
    }
  }
}

// ====== INTERRUPT SERVICE ROUTINE ======
void IRAM_ATTR knockISR() {
  knockDetected = true;
}

// ====== BLE COMMANDS ======
void processCommand(String cmd) {
  cmd.trim();
  Serial.println("Command: " + cmd);

  if (cmd.equalsIgnoreCase("HELP")) {
    String helpText =
      "Commands:\n"
      "SHOW_LOGS (Wi-Fi only)\n"
      "CLEAR_LOGS\n"
      "SET_RTC YYYY-MM-DD HH:MM:SS\n"
      "SYNC_TIME - with RTC\n"
      "SET_NAME\n"
      "SET_LINE_COUNT <number>\n"
      "ADD_NOTE <message>\n"
      "CURRENT_CONFIG\n"
      "SET_TAP 1 or SET_TAP 2\n"
      "SET_SENSITIVITY <1-127>\n";
    pTxCharacteristic->setValue(helpText.c_str());
    pTxCharacteristic->notify();
  }
  else if (cmd.equalsIgnoreCase("SHOW_LOGS")) {
    pTxCharacteristic->setValue("SHOW_LOGS is available via Wi-Fi only.\nConnect to AP and visit http://192.168.4.1/");
    pTxCharacteristic->notify();
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
    pTxCharacteristic->setValue("RTC Time: " + String(rtcTime) + "\n");
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
  else if (cmd.startsWith("ADD_NOTE")) {
    String noteMessage = cmd.substring(9);
    noteMessage.trim();
    if (noteMessage.length() == 0) {
      pTxCharacteristic->setValue("Please provide a message for the note.");
      pTxCharacteristic->notify();
      return;
    }
    logEvent("NOTE " + noteMessage);
    pTxCharacteristic->setValue("Note added: " + noteMessage + "\n");
    pTxCharacteristic->notify();
  }
  else if (cmd.equalsIgnoreCase("CURRENT_CONFIG")) {
    DateTime now = rtc.now();
    char rtcTime[30];
    sprintf(rtcTime, "%04d-%02d-%02d %02d:%02d:%02d",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute(), now.second());
    float tempC = rtc.getTemperature();
    String configText =
      "===== Current Configuration =====\n"
      "Trap Name: " + config.trapName + "\n" +
      "RTC Time: " + String(rtcTime) + "\n" +
      "DS3231 Temp: " + String(tempC, 2) + " C\n" +
      "Max Log Lines: " + String(config.lineCount) + "\n" +
      "Tap Detection: " + String(config.tapCount == 1 ? "Single Tap" : "Double Tap") + "\n" +
      "Tap Sensitivity: " + String(config.sensitivity) + "\n";
    pTxCharacteristic->setValue(configText.c_str());
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
        lis.setClick(tapCount, sensitivity);
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
        lis.setClick(tapCount, sensitivity);
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
  config.tapCount = doc["tapCount"] | 2;
  config.sensitivity = doc["sensitivity"] | 20;
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
  float tempC = rtc.getTemperature();
  char timeStr[40];
  snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  logFile.printf("%s - %s - %s - Temp: %.2f C\n",
                 config.trapName.c_str(), timeStr, message.c_str(), tempC);
  Serial.printf("Wrote log: %s - %s - %s - Temp: %.2f C\n",
                config.trapName.c_str(), timeStr, message.c_str(), tempC);
  logFile.close();
  enforceLogLimit();
}

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

void loadSettings() {
  File settingsFile = SPIFFS.open(configFilePath, "r");
  if (settingsFile) {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, settingsFile);
    if (!error) {
      config.tapCount = doc["tapCount"] | 0;
      config.sensitivity = doc["sensitivity"] | 127;
    } else {
      Serial.println("Failed to parse config file.");
    }
    settingsFile.close();
  } else {
    Serial.println("Failed to open config file for reading.");
  }
}

// ====== TIME SYNC ======
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

// ====== WEB HANDLERS ======
void handleRoot() {
  String page = "<html><head><title>Trap Logger</title></head><body>";
  page += "<h1>" + config.trapName + "</h1>";
  page += "<form action='/show_logs' method='get'><button type='submit'>Show Logs</button></form>";
  page += "<form action='/clear_logs' method='post'><button type='submit'>Clear Logs</button></form>";
  page += "<p>Connect BLE for advanced commands.</p>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handleShowLogs() {
  File logFile = SPIFFS.open(logFilePath, FILE_READ);
  String logContent = "";
  if (!logFile || logFile.size() == 0) {
    server.send(200, "text/plain", "No logs found.");
    if (logFile) logFile.close();
    return;
  }
  while (logFile.available()) {
    logContent += logFile.readStringUntil('\n');
    logContent += "\n";
  }
  logFile.close();
  server.send(200, "text/plain", logContent);
}

void handleClearLogs() {
  SPIFFS.remove(logFilePath);
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Logs cleared. Redirecting...");
}
