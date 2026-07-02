/*
  ESP32-S3 BLE WiFi + HiveMQ MQTT + HTTPS OTA

  Arduino IDE setup:
  - Board: an ESP32-S3 development board
  - Install libraries:
    - ArduinoJson by Benoit Blanchon
    - PubSubClient by Nick O'Leary

  Browser page:
  - Use outputs/index.html from this workspace.
  - Web Bluetooth requires Chrome/Edge and a secure origin:
    https://, or http://localhost.

  Security note:
  This example uses WiFiClientSecure::setInsecure() for easier first testing.
  For production, pin HiveMQ/OTA CA certificates instead.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

static const char *DEVICE_NAME = "ESP32-S3-BLE-OTA";

static const char *SERVICE_UUID = "a7b10000-2c3d-4e5f-9012-3456789abcde";
static const char *CMD_UUID     = "a7b10001-2c3d-4e5f-9012-3456789abcde";
static const char *STATUS_UUID  = "a7b10002-2c3d-4e5f-9012-3456789abcde";

Preferences prefs;
WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

BLECharacteristic *statusCharacteristic = nullptr;
bool bleConnected = false;

String wifiSsid;
String wifiPassword;
String mqttHost;
uint16_t mqttPort = 8883;
String mqttUsername;
String mqttPassword;

unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastStatusPush = 0;

void sendStatus(const String &message) {
  Serial.println(message);
  if (!statusCharacteristic || !bleConnected) {
    return;
  }

  // BLE notifications are safest when kept small. The web page accepts chunks.
  const size_t chunkSize = 180;
  for (size_t i = 0; i < message.length(); i += chunkSize) {
    String chunk = message.substring(i, min(i + chunkSize, message.length()));
    statusCharacteristic->setValue(chunk.c_str());
    statusCharacteristic->notify();
    delay(20);
  }
}

void sendJsonStatus(const char *type, const char *status, const String &message) {
  StaticJsonDocument<320> doc;
  doc["type"] = type;
  doc["status"] = status;
  doc["message"] = message;
  String out;
  serializeJson(doc, out);
  sendStatus(out);
}

String normalizeMqttHost(String url) {
  url.trim();
  url.replace("mqtts://", "");
  url.replace("ssl://", "");
  url.replace("tls://", "");
  url.replace("wss://", "");
  url.replace("ws://", "");

  int slash = url.indexOf('/');
  if (slash >= 0) {
    url = url.substring(0, slash);
  }

  int colon = url.indexOf(':');
  if (colon >= 0) {
    String portText = url.substring(colon + 1);
    int parsedPort = portText.toInt();
    if (parsedPort > 0 && parsedPort <= 65535) {
      mqttPort = (uint16_t)parsedPort;
    }
    url = url.substring(0, colon);
  }

  url.trim();
  return url;
}

void loadSettings() {
  prefs.begin("esp-config", false);
  wifiSsid = prefs.getString("wifi_ssid", "");
  wifiPassword = prefs.getString("wifi_pass", "");
  mqttHost = prefs.getString("mqtt_host", "");
  mqttPort = prefs.getUShort("mqtt_port", 8883);
  mqttUsername = prefs.getString("mqtt_user", "");
  mqttPassword = prefs.getString("mqtt_pass", "");
}

void saveWifiSettings(const String &ssid, const String &password) {
  wifiSsid = ssid;
  wifiPassword = password;
  prefs.putString("wifi_ssid", wifiSsid);
  prefs.putString("wifi_pass", wifiPassword);
}

void saveMqttSettings(String url, const String &username, const String &password, uint16_t port) {
  mqttPort = port > 0 ? port : 8883;
  mqttHost = normalizeMqttHost(url);
  mqttUsername = username;
  mqttPassword = password;
  prefs.putString("mqtt_host", mqttHost);
  prefs.putUShort("mqtt_port", mqttPort);
  prefs.putString("mqtt_user", mqttUsername);
  prefs.putString("mqtt_pass", mqttPassword);
}

bool connectWifi(uint32_t timeoutMs = 20000) {
  if (wifiSsid.isEmpty()) {
    sendJsonStatus("wifi", "error", "WiFi SSID is empty.");
    return false;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == wifiSsid) {
    sendJsonStatus("wifi", "ok", "WiFi already connected. IP: " + WiFi.localIP().toString());
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  sendJsonStatus("wifi", "info", "Connecting to WiFi: " + wifiSsid);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    sendJsonStatus("wifi", "ok", "Connected. IP: " + WiFi.localIP().toString());
    return true;
  }

  sendJsonStatus("wifi", "error", "WiFi connection timed out.");
  return false;
}

void scanWifiNetworks() {
  sendJsonStatus("wifi_scan", "info", "Scanning WiFi networks...");
  WiFi.mode(WIFI_STA);
  int count = WiFi.scanNetworks(false, true);

  if (count < 0) {
    sendJsonStatus("wifi_scan", "error", "WiFi scan failed.");
    return;
  }

  for (int i = 0; i < count; i++) {
    StaticJsonDocument<384> doc;
    doc["type"] = "wifi_network";
    doc["ssid"] = WiFi.SSID(i);
    doc["rssi"] = WiFi.RSSI(i);
    doc["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    String out;
    serializeJson(doc, out);
    sendStatus(out);
  }

  StaticJsonDocument<160> done;
  done["type"] = "wifi_scan_done";
  done["count"] = count;
  String out;
  serializeJson(done, out);
  sendStatus(out);
}

bool connectMqtt(bool announce = true) {
  if (mqttHost.isEmpty()) {
    sendJsonStatus("mqtt", "error", "MQTT host is empty.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return false;
  }

  tlsClient.setInsecure();
  mqtt.setServer(mqttHost.c_str(), mqttPort);

  String clientId = "esp32-s3-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok;
  if (mqttUsername.length()) {
    ok = mqtt.connect(clientId.c_str(), mqttUsername.c_str(), mqttPassword.c_str());
  } else {
    ok = mqtt.connect(clientId.c_str());
  }

  if (ok) {
    String topic = "esp32-s3/" + String((uint32_t)ESP.getEfuseMac(), HEX) + "/status";
    mqtt.publish(topic.c_str(), "online", true);
    if (announce) {
      sendJsonStatus("mqtt", "ok", "Connected to " + mqttHost + ":" + String(mqttPort));
    }
    return true;
  }

  if (announce) {
    sendJsonStatus("mqtt", "error", "Connect failed. PubSubClient state: " + String(mqtt.state()));
  }
  return false;
}

bool performOta(const String &url) {
  if (url.isEmpty()) {
    sendJsonStatus("ota", "error", "OTA URL is empty.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    return false;
  }

  sendJsonStatus("ota", "info", "Starting OTA download...");

  WiFiClientSecure otaClient;
  otaClient.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(otaClient, url)) {
    sendJsonStatus("ota", "error", "Unable to open OTA URL.");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    sendJsonStatus("ota", "error", "HTTP GET failed: " + String(httpCode));
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    sendJsonStatus("ota", "error", "Invalid firmware size.");
    http.end();
    return false;
  }

  if (!Update.begin(contentLength)) {
    sendJsonStatus("ota", "error", "Not enough space for OTA.");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written != (size_t)contentLength) {
    sendJsonStatus("ota", "error", "OTA write incomplete.");
    http.end();
    return false;
  }

  if (!Update.end()) {
    sendJsonStatus("ota", "error", "OTA end failed: " + String(Update.getError()));
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    sendJsonStatus("ota", "error", "OTA did not finish.");
    http.end();
    return false;
  }

  http.end();
  sendJsonStatus("ota", "ok", "OTA complete. Rebooting...");
  delay(1000);
  ESP.restart();
  return true;
}

void handleCommand(const String &payload) {
  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    sendJsonStatus("command", "error", "Invalid JSON command.");
    return;
  }

  String action = doc["action"] | "";

  if (action == "scan_wifi") {
    scanWifiNetworks();
    return;
  }

  if (action == "save_wifi") {
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    saveWifiSettings(ssid, password);
    sendJsonStatus("wifi", "ok", "WiFi settings saved.");
    connectWifi();
    return;
  }

  if (action == "save_mqtt") {
    String url = doc["url"] | "";
    String username = doc["username"] | "";
    String password = doc["password"] | "";
    uint16_t port = doc["port"] | 8883;
    saveMqttSettings(url, username, password, port);
    sendJsonStatus("mqtt", "ok", "MQTT settings saved.");
    connectMqtt();
    return;
  }

  if (action == "connect_mqtt") {
    connectMqtt();
    return;
  }

  if (action == "ota") {
    String url = doc["url"] | "";
    performOta(url);
    return;
  }

  if (action == "status") {
    StaticJsonDocument<384> status;
    status["type"] = "device_status";
    status["wifi"] = WiFi.status() == WL_CONNECTED;
    status["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
    status["mqtt"] = mqtt.connected();
    status["mqttHost"] = mqttHost;
    status["mqttPort"] = mqttPort;
    String out;
    serializeJson(status, out);
    sendStatus(out);
    return;
  }

  sendJsonStatus("command", "error", "Unknown action: " + action);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleConnected = true;
    sendJsonStatus("ble", "ok", "Browser connected.");
  }

  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    delay(300);
    BLEDevice::startAdvertising();
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String payload = characteristic->getValue().c_str();
    payload.trim();
    if (payload.length()) {
      handleCommand(payload);
    }
  }
};

void setupBle() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(517);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);

  BLECharacteristic *cmdCharacteristic = service->createCharacteristic(
    CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  cmdCharacteristic->setCallbacks(new CommandCallbacks());

  statusCharacteristic = service->createCharacteristic(
    STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  statusCharacteristic->addDescriptor(new BLE2902());
  statusCharacteristic->setValue("ESP32-S3 ready");

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  loadSettings();
  setupBle();

  sendJsonStatus("boot", "ok", "ESP32-S3 BLE OTA ready.");

  if (wifiSsid.length()) {
    connectWifi(12000);
  }
  if (WiFi.status() == WL_CONNECTED && mqttHost.length()) {
    connectMqtt(false);
  }
}

void loop() {
  if (mqtt.connected()) {
    mqtt.loop();
  } else if (WiFi.status() == WL_CONNECTED && mqttHost.length()) {
    if (millis() - lastMqttReconnectAttempt > 15000) {
      lastMqttReconnectAttempt = millis();
      connectMqtt(false);
    }
  }

  if (millis() - lastStatusPush > 30000) {
    lastStatusPush = millis();
    if (bleConnected) {
      StaticJsonDocument<256> doc;
      doc["type"] = "heartbeat";
      doc["wifi"] = WiFi.status() == WL_CONNECTED;
      doc["mqtt"] = mqtt.connected();
      doc["freeHeap"] = ESP.getFreeHeap();
      String out;
      serializeJson(doc, out);
      sendStatus(out);
    }
  }
}
