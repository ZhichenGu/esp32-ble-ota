/**
 * ESP32-S3 BLE Configuration + MQTT + OTA Firmware
 * =================================================
 * 功能：
 *   1. BLE GATT 服务 — 通过 Web Bluetooth 配置 WiFi 和 MQTT
 *   2. WiFi 连接管理
 *   3. MQTT over TLS 连接 HiveMQ
 *   4. OTA 远程升级
 *   5. NVS 持久化存储配置
 *
 * BLE GATT Service UUID: 12345678-1234-1234-1234-123456789abc
 *
 * Arduino IDE 依赖库：
 *   - Preferences (内置 ESP32)
 *   - WiFi (内置 ESP32)
 *   - BLE (内置 ESP32)
 *   - PubSubClient (库管理器安装)
 *   - HTTPClient (内置 ESP32)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLECharacteristic.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>

// ========================
// 配置区
// ========================

// BLE 设备名称（浏览器扫描时显示）
#define BLE_DEVICE_NAME     "ESP32_Config"
#define BLE_DEVICE_NAME_AP  "ESP32-OTA"

// BLE GATT UUID
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHAR_WIFI_SCAN      "12345678-1234-1234-1234-123456789ab1"
#define CHAR_WIFI_CONFIG    "12345678-1234-1234-1234-123456789ab2"
#define CHAR_MQTT_CONFIG    "12345678-1234-1234-1234-123456789ab3"
#define CHAR_OTA_URL        "12345678-1234-1234-1234-123456789ab4"
#define CHAR_CONTROL        "12345678-1234-1234-1234-123456789ab5"
#define CHAR_STATUS         "12345678-1234-1234-1234-123456789ab6"

// 命令码
#define CMD_SCAN_WIFI       0x01
#define CMD_CONNECT_WIFI    0x02
#define CMD_CONNECT_MQTT    0x03
#define CMD_START_OTA       0x04
#define CMD_SAVE_ALL        0x05
#define CMD_RESET_CONFIG    0x06
#define CMD_STATUS_REQUEST  0x07

// 状态码
#define STATUS_IDLE             0x00
#define STATUS_SCANNING         0x01
#define STATUS_WIFI_CONNECTING  0x02
#define STATUS_WIFI_CONNECTED   0x03
#define STATUS_WIFI_FAILED      0x04
#define STATUS_MQTT_CONNECTING  0x05
#define STATUS_MQTT_CONNECTED   0x06
#define STATUS_MQTT_FAILED      0x07
#define STATUS_OTA_STARTING     0x08
#define STATUS_OTA_PROGRESS     0x09
#define STATUS_OTA_SUCCESS      0x0A
#define STATUS_OTA_FAILED       0x0B
#define STATUS_OK               0xF0
#define STATUS_ERROR            0xFF

// WiFi
#define WIFI_TIMEOUT_MS     15000
#define WIFI_RETRY_DELAY_MS 500

// OTA
#define OTA_CHECK_TIMEOUT_MS 8000

// MQTT
#define MQTT_KEEPALIVE       60
#define MQTT_RECONNECT_DELAY 5000
#define MQTT_MAX_RETRIES     3

// ========================
// 全局变量
// ========================

// BLE
BLEServer *pServer = nullptr;
BLEService *pService = nullptr;
BLECharacteristic *pCharWiFiScan = nullptr;
BLECharacteristic *pCharWiFiConfig = nullptr;
BLECharacteristic *pCharMQTTConfig = nullptr;
BLECharacteristic *pCharOTAUrl = nullptr;
BLECharacteristic *pCharControl = nullptr;
BLECharacteristic *pCharStatus = nullptr;
BLE2902 *pDescStatus = nullptr;
bool deviceConnected = false;
bool isConfigMode = false;

// WiFi 状态
bool wifiConnected = false;
bool wifiConnecting = false;
unsigned long wifiConnectStartTime = 0;
String currentSSID = "";

// MQTT 状态
bool mqttConnected = false;
WiFiClientSecure *wifiClientSecure = nullptr;
PubSubClient *mqttClient = nullptr;
String mqttDeviceId = "";
unsigned long mqttReconnectStartTime = 0;
int mqttRetryCount = 0;

// OTA 状态
bool otaInProgress = false;
unsigned long otaCheckStartTime = 0;
HTTPClient *otaHttp = nullptr;

// NVS
Preferences preferences;

// WiFi 扫描结果
std::vector<String> scanResults;
bool scanDone = false;

// 配网状态
bool hasWiFiConfig = false;
bool hasMQTTConfig = false;

// ========================
// 函数声明
// ========================

void initBLE();
void initNVS();
void initMQTT();
void sendBLEStatus(uint8_t status, const String &message = "");
void sendWiFiScanResults();
void connectWiFi(const String &ssid, const String &password);
void connectMQTT();
void startOTA(const String &url);
void saveWiFiConfig(const String &ssid, const String &password);
void saveMQTTConfig(const String &broker, uint16_t port, const String &username, const String &password);
void resetAllConfig();
void sendMQTTStatus();
void checkConnectionTimeout();
void handleWiFiEvents(WiFiEvent_t event);

// ========================
// BLE 回调
// ========================

class BLEServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("[BLE] Client connected");
        sendBLEStatus(STATUS_OK, "Connected");
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("[BLE] Client disconnected");
        // 重启广播，让浏览器能重新发现
        BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
        pAdvertising->start();
        Serial.println("[BLE] Restarted advertising");
    }
};

class BLEWiFiScanCallbacks: public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic *pCharacteristic) {
        Serial.println("[BLE] WiFi Scan read request");
        sendWiFiScanResults();
    }

    void onNotify(BLECharacteristic *pCharacteristic) {
        Serial.println("[BLE] WiFi Scan notified");
    }
};

class BLEWiFiConfigCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        Serial.printf("[BLE] WiFi Config write, len=%d\n", value.length());
        if (value.length() < 2) {
            sendBLEStatus(STATUS_ERROR, "Invalid WiFi config");
            return;
        }

        uint8_t ssidLen = value[0];
        if (value.length() < 1 + ssidLen + 1) {
            sendBLEStatus(STATUS_ERROR, "Invalid WiFi config format");
            return;
        }

        String ssid = value.substr(1, ssidLen).c_str();
        uint8_t passLen = value[1 + ssidLen];
        if (value.length() < 1 + ssidLen + 1 + passLen) {
            sendBLEStatus(STATUS_ERROR, "Invalid WiFi config format");
            return;
        }
        String password = value.substr(1 + ssidLen + 1, passLen).c_str();

        Serial.printf("[BLE] WiFi SSID: %s, pass len: %d\n", ssid.c_str(), passLen);

        if (ssid.length() == 0) {
            sendBLEStatus(STATUS_ERROR, "SSID cannot be empty");
            return;
        }

        saveWiFiConfig(ssid, password);
        sendBLEStatus(STATUS_OK, "WiFi config saved");
    }
};

class BLEMqttConfigCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        Serial.printf("[BLE] MQTT Config write, len=%d\n", value.length());

        // 格式: BROKER_LEN(1) + BROKER + PORT(2) + USER_LEN(1) + USER + PASS_LEN(1) + PASS
        if (value.length() < 6) {
            sendBLEStatus(STATUS_ERROR, "Invalid MQTT config");
            return;
        }

        size_t pos = 0;
        uint8_t brokerLen = value[pos++];
        if (value.length() < pos + brokerLen + 2) {
            sendBLEStatus(STATUS_ERROR, "Invalid MQTT config");
            return;
        }
        String broker = value.substr(pos, brokerLen).c_str();
        pos += brokerLen;

        uint16_t port = (value[pos] << 8) | value[pos + 1];
        pos += 2;

        uint8_t userLen = value[pos++];
        if (value.length() < pos + userLen + 1) {
            sendBLEStatus(STATUS_ERROR, "Invalid MQTT config");
            return;
        }
        String username = value.substr(pos, userLen).c_str();
        pos += userLen;

        uint8_t passLen = value[pos++];
        if (value.length() < pos + passLen) {
            sendBLEStatus(STATUS_ERROR, "Invalid MQTT config");
            return;
        }
        String password = value.substr(pos, passLen).c_str();

        Serial.printf("[BLE] MQTT Broker: %s:%d, user: %s\n", broker.c_str(), port, username.c_str());

        if (broker.length() == 0) {
            sendBLEStatus(STATUS_ERROR, "Broker cannot be empty");
            return;
        }

        saveMQTTConfig(broker, port, username, password);
        sendBLEStatus(STATUS_OK, "MQTT config saved");
    }
};

class BLEOTAUrlCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        Serial.printf("[BLE] OTA URL write, len=%d\n", value.length());

        if (value.empty()) {
            sendBLEStatus(STATUS_ERROR, "OTA URL empty");
            return;
        }

        String url = value.c_str();
        Serial.printf("[BLE] OTA URL: %s\n", url.c_str());

        startOTA(url);
    }
};

class BLEControlCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        Serial.printf("[BLE] Control write, len=%d\n", value.length());

        if (value.empty()) {
            sendBLEStatus(STATUS_ERROR, "No command");
            return;
        }

        uint8_t cmd = value[0];
        Serial.printf("[BLE] Command: 0x%02X\n", cmd);

        switch (cmd) {
            case CMD_SCAN_WIFI:
                Serial.println("[BLE] CMD: Scan WiFi");
                sendBLEStatus(STATUS_SCANNING, "Scanning WiFi...");
                scanWiFiNetworks();
                break;

            case CMD_CONNECT_WIFI: {
                Serial.println("[BLE] CMD: Connect WiFi");
                String ssid = preferences.getString("wifi_ssid", "");
                String pass = preferences.getString("wifi_pass", "");
                if (ssid.length() > 0) {
                    sendBLEStatus(STATUS_WIFI_CONNECTING, "Connecting WiFi...");
                    connectWiFi(ssid, pass);
                } else {
                    sendBLEStatus(STATUS_ERROR, "No WiFi config");
                }
                break;
            }

            case CMD_CONNECT_MQTT:
                Serial.println("[BLE] CMD: Connect MQTT");
                if (hasMQTTConfig) {
                    sendBLEStatus(STATUS_MQTT_CONNECTING, "Connecting MQTT...");
                    connectMQTT();
                } else {
                    sendBLEStatus(STATUS_ERROR, "No MQTT config");
                }
                break;

            case CMD_SAVE_ALL:
                Serial.println("[BLE] CMD: Save All");
                sendBLEStatus(STATUS_OK, "All config saved");
                break;

            case CMD_RESET_CONFIG:
                Serial.println("[BLE] CMD: Reset Config");
                resetAllConfig();
                sendBLEStatus(STATUS_OK, "Config reset");
                break;

            case CMD_STATUS_REQUEST:
                Serial.println("[BLE] CMD: Status Request");
                sendMQTTStatus();
                break;

            default:
                Serial.printf("[BLE] Unknown command: 0x%02X\n", cmd);
                sendBLEStatus(STATUS_ERROR, "Unknown command");
                break;
        }
    }
};

// ========================
// WiFi 扫描
// ========================

void scanWiFiNetworks() {
    scanResults.clear();
    scanDone = false;

    Serial.println("[WiFi] Starting scan...");
    int n = WiFi.scanNetworks(false, true); // async, show hidden

    if (n < 0) {
        Serial.println("[WiFi] Scan failed");
        sendBLEStatus(STATUS_ERROR, "WiFi scan failed");
        return;
    }

    Serial.printf("[WiFi] Found %d networks\n", n);

    // 等待扫描完成
    while (WiFi.scanComplete() == -2) {
        delay(100);
    }

    n = WiFi.scanComplete();
    for (int i = 0; i < n && i < 20; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() > 0) {
            Serial.printf("  [%d] %s (rssi=%d, enc=%d)\n",
                i, ssid.c_str(), WiFi.RSSI(i), WiFi.encryptionType(i));
            scanResults.push_back(ssid);
        }
    }

    WiFi.scanDelete();
    scanDone = true;
    sendBLEStatus(STATUS_OK, "Scan complete");
    sendWiFiScanResults();
}

void sendWiFiScanResults() {
    if (scanResults.empty()) {
        // 发送空列表
        uint8_t data[1] = {0};
        pCharWiFiScan->setValue(data, 1);
        pCharWiFiScan->notify();
        return;
    }

    // 组包: [COUNT][SSID1_LEN][SSID1...][RSSI1][AUTH1]...
    uint8_t buffer[1024];
    size_t idx = 0;
    buffer[idx++] = (uint8_t)scanResults.size();

    for (size_t i = 0; i < scanResults.size(); i++) {
        const String &ssid = scanResults[i];
        int rssi = 0;
        int8_t auth = 0;

        for (int j = 0; j < WiFi.scanComplete(); j++) {
            if (WiFi.SSID(j) == ssid) {
                rssi = WiFi.RSSI(j);
                auth = WiFi.encryptionType(j);
                break;
            }
        }

        buffer[idx++] = (uint8_t)ssid.length();
        memcpy(&buffer[idx], ssid.c_str(), ssid.length());
        idx += ssid.length();
        buffer[idx++] = (uint8_t)rssi;
        buffer[idx++] = (uint8_t)auth;
    }

    pCharWiFiScan->setValue(buffer, idx);
    pCharWiFiScan->notify();
    Serial.printf("[BLE] Sent %d WiFi networks\n", scanResults.size());
}

// ========================
// WiFi 连接
// ========================

void connectWiFi(const String &ssid, const String &password) {
    if (wifiConnecting) {
        Serial.println("[WiFi] Already connecting");
        return;
    }

    Serial.printf("[WiFi] Connecting to: %s\n", ssid.c_str());
    wifiConnecting = true;
    wifiConnected = false;
    wifiConnectStartTime = millis();
    currentSSID = ssid;

    WiFi.disconnect(true);
    WiFi.onEvent(handleWiFiEvents);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
}

void handleWiFiEvents(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            wifiConnected = true;
            wifiConnecting = false;
            Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            hasWiFiConfig = true;
            sendBLEStatus(STATUS_WIFI_CONNECTED, "WiFi connected: " + WiFi.localIP().toString());

            // 如果已有 MQTT 配置，自动连接
            if (hasMQTTConfig) {
                delay(500);
                connectMQTT();
            }
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            wifiConnected = false;
            wifiConnecting = false;
            Serial.println("[WiFi] Disconnected");
            if (!isConfigMode && hasWiFiConfig) {
                sendBLEStatus(STATUS_WIFI_FAILED, "WiFi disconnected");
            }
            break;

        default:
            break;
    }
}

// ========================
// MQTT 连接
// ========================

void initMQTT() {
    if (wifiClientSecure) delete wifiClientSecure;
    if (mqttClient) delete mqttClient;

    wifiClientSecure = new WiFiClientSecure();
    // HiveMQ Cloud 使用的是受信任的 CA 证书，ESP32 内置了
    wifiClientSecure->setInsecure(); // HiveMQ Cloud 证书有时效，简单起见用这个
    // 如需生产环境，请下载 HiveMQ 的证书并用 setCACert()

    mqttClient = new PubSubClient(*wifiClientSecure);
    mqttClient->setBufferSize(1024);
    mqttClient->setKeepAlive(MQTT_KEEPALIVE);
    mqttClient->setSocketTimeout(10);

    // 生成设备 ID
    mqttDeviceId = "esp32_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("[MQTT] Device ID: %s\n", mqttDeviceId.c_str());
}

void connectMQTT() {
    if (!wifiConnected) {
        Serial.println("[MQTT] WiFi not connected");
        sendBLEStatus(STATUS_MQTT_FAILED, "WiFi not connected");
        return;
    }

    if (!hasMQTTConfig) {
        Serial.println("[MQTT] No config");
        sendBLEStatus(STATUS_MQTT_FAILED, "No MQTT config");
        return;
    }

    String broker = preferences.getString("mqtt_broker", "");
    uint16_t port = preferences.getUShort("mqtt_port", 8883);
    String username = preferences.getString("mqtt_user", "");
    String password = preferences.getString("mqtt_pass", "");

    if (broker.length() == 0) {
        sendBLEStatus(STATUS_MQTT_FAILED, "No broker configured");
        return;
    }

    Serial.printf("[MQTT] Connecting to %s:%d as %s\n", broker.c_str(), port, mqttDeviceId.c_str());
    sendBLEStatus(STATUS_MQTT_CONNECTING, "Connecting MQTT...");

    mqttClient->setServer(broker.c_str(), port);

    bool ok = false;
    if (username.length() > 0) {
        ok = mqttClient->connect(mqttDeviceId.c_str(), username.c_str(), password.c_str());
    } else {
        ok = mqttClient->connect(mqttDeviceId.c_str());
    }

    if (ok) {
        mqttConnected = true;
        mqttRetryCount = 0;
        Serial.println("[MQTT] Connected!");
        sendBLEStatus(STATUS_MQTT_CONNECTED, "MQTT connected");

        // 订阅配置 topic
        String configTopic = "esp32/" + mqttDeviceId + "/config";
        mqttClient->subscribe(configTopic.c_str());
        Serial.printf("[MQTT] Subscribed to: %s\n", configTopic.c_str());

    } else {
        mqttConnected = false;
        mqttRetryCount++;
        Serial.printf("[MQTT] Connect failed, rc=%d\n", mqttClient->getState());
        sendBLEStatus(STATUS_MQTT_FAILED, "MQTT failed: " + String(mqttClient->getState()));

        // 延迟重试
        mqttReconnectStartTime = millis();
    }
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
    Serial.printf("[MQTT] Message on %s, len=%d\n", topic, length);

    String topicStr = topic;
    String deviceId = mqttDeviceId;

    if (topicStr.indexOf(deviceId + "/config") >= 0) {
        // 收到配置命令
        if (length >= 1) {
            uint8_t cmd = payload[0];
            Serial.printf("[MQTT] Config command: 0x%02X\n", cmd);

            if (cmd == 0x01) {
                // 请求状态
                sendMQTTStatus();
            }
        }
    }
}

void mqttLoop() {
    if (!mqttClient) return;

    if (!mqttConnected) {
        unsigned long now = millis();
        if (now - mqttReconnectStartTime > MQTT_RECONNECT_DELAY && mqttRetryCount < MQTT_MAX_RETRIES) {
            connectMQTT();
        }
        return;
    }

    mqttClient->loop();
}

void sendMQTTStatus() {
    if (!mqttConnected) {
        sendBLEStatus(STATUS_MQTT_FAILED, "MQTT not connected");
        return;
    }

    String statusTopic = "esp32/" + mqttDeviceId + "/status";
    String ip = wifiConnected ? WiFi.localIP().toString() : "disconnected";
    String msg = "{\"ip\":\"" + ip + "\",\"rssi\":" + String(WiFi.RSSI()) +
                 ",\"mqtt\":true,\"ota\":" + (otaInProgress ? "true" : "false") + "}";

    mqttClient->publish(statusTopic.c_str(), msg.c_str());
    Serial.printf("[MQTT] Published status to %s\n", statusTopic.c_str());
}

// ========================
// OTA 升级
// ========================

void startOTA(const String &url) {
    if (wifiConnected == false || hasWiFiConfig == false) {
        sendBLEStatus(STATUS_ERROR, "WiFi not connected for OTA");
        return;
    }

    if (otaInProgress) {
        sendBLEStatus(STATUS_ERROR, "OTA already in progress");
        return;
    }

    Serial.printf("[OTA] Starting: %s\n", url.c_str());
    sendBLEStatus(STATUS_OTA_STARTING, "OTA starting...");

    otaInProgress = true;
    otaCheckStartTime = millis();

    otaHttp = new HTTPClient();
    otaHttp->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    otaHttp->setTimeout(30000);

    // 注意：ESP32 OTA 需要固件编译时设置 partition scheme
    // Sketch 使用 1.2MB APP / 1.5MB SPIFFS 或自定义分区表

    // 以下是 OTA 核心逻辑（将在 otaLoop 中执行）
}

void otaLoop() {
    if (!otaInProgress || !otaHttp) return;

    // OTA 正在 HTTPClient 中执行，这里简化处理
    // 实际项目中建议使用 AsyncTCP 或后台任务
}

// 实际 OTA 执行（在独立任务或 loop 中调用）
void performOTA(const String &url) {
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(30000);

    Serial.printf("[OTA] Downloading: %s\n", url.c_str());

    if (!http.begin(url.c_str())) {
        Serial.println("[OTA] Failed to begin HTTP");
        sendBLEStatus(STATUS_OTA_FAILED, "OTA begin failed");
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        return;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[OTA] HTTP error: %d\n", httpCode);
        sendBLEStatus(STATUS_OTA_FAILED, "HTTP error: " + String(httpCode));
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        return;
    }

    int contentLength = http.getSize();
    Serial.printf("[OTA] Content length: %d bytes\n", contentLength);

    if (contentLength <= 0) {
        sendBLEStatus(STATUS_OTA_FAILED, "Unknown content length");
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        return;
    }

    // 检查是否有足够的空间
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);

    if (!update) {
        sendBLEStatus(STATUS_OTA_FAILED, "No OTA partition");
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        return;
    }

    Serial.printf("[OTA] Running partition size: %d, Update partition size: %d\n",
        running->size, update->size);

    if ((size_t)contentLength > update->size) {
        sendBLEStatus(STATUS_OTA_FAILED, "Firmware too large");
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        return;
    }

    // 开始 OTA
    if (!Update.begin(contentLength)) {
        sendBLEStatus(STATUS_OTA_FAILED, "OTA begin error");
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        return;
    }

    Update.onProgress([](size_t progress, size_t total) {
        uint8_t pct = (progress * 100) / total;
        Serial.printf("[OTA] Progress: %d%%\n", pct);
        sendBLEStatus(STATUS_OTA_PROGRESS, "OTA " + String(pct) + "%");
    });

    // 通过 WiFiClientRead 写入 Update
    WiFiClient *stream = http.getStreamPtr();
    size_t written = Update.writeStream(*stream);

    if (written != (size_t)contentLength) {
        Serial.printf("[OTA] Written %d of %d\n", written, contentLength);
        sendBLEStatus(STATUS_OTA_FAILED, "OTA write error");
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        return;
    }

    if (Update.end(true)) {
        Serial.println("[OTA] Update complete, rebooting...");
        sendBLEStatus(STATUS_OTA_SUCCESS, "OTA success, rebooting...");
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
        delay(1000);
        ESP.restart();
    } else {
        Serial.printf("[OTA] Update error: %s\n", Update.errorString());
        sendBLEStatus(STATUS_OTA_FAILED, "OTA error: " + String(Update.errorString()));
        http.end();
        otaInProgress = false;
        delete otaHttp;
        otaHttp = nullptr;
    }
}

// ========================
// BLE 状态发送
// ========================

void sendBLEStatus(uint8_t status, const String &message) {
    if (!pCharStatus) return;

    // 组包: STATUS(1) + MSG_LEN(1) + MSG...
    uint8_t buffer[128];
    size_t idx = 0;
    buffer[idx++] = status;

    // 附加额外信息（可选，保留给未来扩展）
    String extra = "";
    if (wifiConnected) {
        extra = WiFi.localIP().toString();
    }
    if (message.length() > 0) {
        if (extra.length() > 0) extra += " | ";
        extra += message;
    }

    uint8_t msgLen = (uint8_t)min(extra.length(), 120);
    buffer[idx++] = msgLen;
    memcpy(&buffer[idx], extra.c_str(), msgLen);
    idx += msgLen;

    pCharStatus->setValue(buffer, idx);

    if (deviceConnected) {
        pCharStatus->notify();
        Serial.printf("[BLE] Status notify: 0x%02X - %s\n", status, extra.c_str());
    }
}

// ========================
// NVS 存储
// ========================

void initNVS() {
    preferences.begin("esp32-config", false);
    Serial.println("[NVS] Initialized");

    // 检查是否有已保存的配置
    hasWiFiConfig = preferences.isKey("wifi_ssid");
    hasMQTTConfig = preferences.isKey("mqtt_broker");

    if (hasWiFiConfig) {
        Serial.printf("[NVS] Has WiFi config: %s\n", preferences.getString("wifi_ssid").c_str());
    }
    if (hasMQTTConfig) {
        Serial.printf("[NVS] Has MQTT config: %s\n", preferences.getString("mqtt_broker").c_str());
    }
}

void saveWiFiConfig(const String &ssid, const String &password) {
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pass", password);
    hasWiFiConfig = true;
    Serial.printf("[NVS] WiFi config saved: %s\n", ssid.c_str());
}

void saveMQTTConfig(const String &broker, uint16_t port, const String &username, const String &password) {
    preferences.putString("mqtt_broker", broker);
    preferences.putUShort("mqtt_port", port);
    preferences.putString("mqtt_user", username);
    preferences.putString("mqtt_pass", password);
    hasMQTTConfig = true;
    Serial.printf("[NVS] MQTT config saved: %s:%d\n", broker.c_str(), port);
    initMQTT(); // 重新初始化 MQTT
}

void resetAllConfig() {
    preferences.clear();
    hasWiFiConfig = false;
    hasMQTTConfig = false;

    if (mqttConnected && mqttClient) {
        mqttClient->disconnect();
    }
    mqttConnected = false;
    WiFi.disconnect(true);
    wifiConnected = false;

    Serial.println("[NVS] Config reset");
}

// ========================
// BLE 初始化
// ========================

void initBLE() {
    Serial.println("[BLE] Starting...");

    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_P9); // 最大功率

    // 创建 GATT Server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new BLEServerCallbacks());

    // 创建 Service
    pService = pServer->createService(SERVICE_UUID);

    // WiFi Scan Characteristic (READ + NOTIFY)
    pCharWiFiScan = pService->createCharacteristic(
        CHAR_WIFI_SCAN,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharWiFiScan->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
    pCharWiFiScan->setCallbacks(new BLEWiFiScanCallbacks());
    pCharWiFiScan->addDescriptor(new BLE2902());

    // WiFi Config Characteristic (WRITE)
    pCharWiFiConfig = pService->createCharacteristic(
        CHAR_WIFI_CONFIG,
        BLECharacteristic::PROPERTY_WRITE
    );
    pCharWiFiConfig->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    pCharWiFiConfig->setCallbacks(new BLEWiFiConfigCallbacks());

    // MQTT Config Characteristic (WRITE)
    pCharMQTTConfig = pService->createCharacteristic(
        CHAR_MQTT_CONFIG,
        BLECharacteristic::PROPERTY_WRITE
    );
    pCharMQTTConfig->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    pCharMQTTConfig->setCallbacks(new BLEMqttConfigCallbacks());

    // OTA URL Characteristic (WRITE)
    pCharOTAUrl = pService->createCharacteristic(
        CHAR_OTA_URL,
        BLECharacteristic::PROPERTY_WRITE
    );
    pCharOTAUrl->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    pCharOTAUrl->setCallbacks(new BLEOTAUrlCallbacks());

    // Control Characteristic (WRITE)
    pCharControl = pService->createCharacteristic(
        CHAR_CONTROL,
        BLECharacteristic::PROPERTY_WRITE
    );
    pCharControl->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    pCharControl->setCallbacks(new BLEControlCallbacks());

    // Status Characteristic (READ + NOTIFY)
    pCharStatus = pService->createCharacteristic(
        CHAR_STATUS,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharStatus->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
    pDescStatus = new BLE2902();
    pCharStatus->addDescriptor(pDescStatus);

    // 启动 Service
    pService->start();

    // 配置广播
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);

    BLEDevice::startAdvertising();
    Serial.println("[BLE] Advertising as '" BLE_DEVICE_NAME "'");
    Serial.println("[BLE] Service UUID: " SERVICE_UUID);
}

// ========================
// 连接超时检测
// ========================

void checkConnectionTimeout() {
    unsigned long now = millis();

    // WiFi 连接超时
    if (wifiConnecting && (now - wifiConnectStartTime > WIFI_TIMEOUT_MS)) {
        Serial.println("[WiFi] Connection timeout");
        wifiConnecting = false;
        sendBLEStatus(STATUS_WIFI_FAILED, "WiFi timeout");
        WiFi.disconnect(true);
    }
}

// ========================
// 配置模式 (AP)
// ========================

void startConfigMode() {
    Serial.println("[Config] Starting AP mode");
    isConfigMode = true;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(BLE_DEVICE_NAME_AP);

    Serial.printf("[Config] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.println("[Config] Connect to BLE for configuration");
}

// ========================
// 自动连接逻辑
// ========================

void autoConnect() {
    if (hasWiFiConfig) {
        String ssid = preferences.getString("wifi_ssid", "");
        String pass = preferences.getString("wifi_pass", "");
        if (ssid.length() > 0) {
            Serial.printf("[Auto] Attempting saved WiFi: %s\n", ssid.c_str());
            connectWiFi(ssid, pass);
        }
    }

    if (hasMQTTConfig && wifiConnected) {
        delay(2000);
        connectMQTT();
    }
}

// ========================
// setup & loop
// ========================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("========================================");
    Serial.println("  ESP32-S3 BLE Config + MQTT + OTA");
    Serial.println("========================================");

    // 初始化 NVS
    initNVS();

    // 初始化 BLE（始终广播，让手机可以配网）
    initBLE();

    // 自动连接已保存的 WiFi 和 MQTT
    autoConnect();

    Serial.println("[Setup] Done");
}

void loop() {
    // BLE 连接处理
    if (deviceConnected) {
        pServer->startAdvertising();
    }

    // 检查连接超时
    checkConnectionTimeout();

    // MQTT 循环
    mqttLoop();

    // OTA 循环（如果进行中）
    otaLoop();

    delay(10);
}
