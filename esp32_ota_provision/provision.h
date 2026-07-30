/*
 * provision.h  —  可复用的 ESP32 "BLE 配网 + WiFi + HiveMQ MQTT + 空中升级(OTA)" 核心
 * ============================================================================
 *  【最重要的一件事】
 *  把这个文件放进任何一个 sketch 目录, 然后在你的 .ino 里:
 *
 *      #include "provision.h"
 *      void setup(){ provisionSetup("ESP32-XXX"); ...你的初始化... }
 *      void loop(){  provisionLoop();             ...你的业务逻辑... }
 *
 *  只要你以后【每一个】要 OTA 上去的固件都 #include 这个文件,
 *  "扫蓝牙 / 配 WiFi / 配 MQTT / OTA" 的能力就【永远不会】因为一次 OTA 而丢失。
 *  这就是对"刷了新固件功能会不会没了"的正解。
 * ============================================================================
 *  依赖库 (Arduino 库管理器安装, BLE/Update/HTTPUpdate 随 ESP32 开发板包自带):
 *    - PubSubClient   by Nick O'Leary
 *    - ArduinoJson    by Benoit Blanchon (v7)
 *  分区: 必须用带 OTA 的分区表(本目录 partitions.csv), 详见 README。
 */
#pragma once

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPUpdate.h>

// ---------------- 可用 #define 覆盖的编译期配置 ----------------
#ifndef PROV_TOPIC_PREFIX
#define PROV_TOPIC_PREFIX "esp32"      // MQTT 主题前缀, 须与网页一致
#endif
#ifndef PROV_AUTO_MQTT_MS
#define PROV_AUTO_MQTT_MS 0            // >0: 开机 N 毫秒内无 BLE 连接则自动连 MQTT(会释放蓝牙,进入"运行态")
#endif                                 //  0: 永不自动, 始终等 BLE 指令(OTA 场景推荐)

// ---------------- BLE UUID (Nordic UART + 一路 OTA 数据) ----------------
#define PROV_SVC_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define PROV_RX_UUID  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // 网页 -> ESP32 (write, JSON 命令)
#define PROV_TX_UUID  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // ESP32 -> 网页 (notify, JSON 事件)
#define PROV_OTA_UUID "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  // 网页 -> ESP32 (write, 固件原始字节)

// ---------------- 全局对象 (本头文件只被 .ino 包含一次) ----------------
static BLECharacteristic* provTx = nullptr;
static bool provBleConnected = false;
static bool provBleFreed     = false;

WiFiClientSecure provNet;
PubSubClient     mqtt(provNet);        // 业务代码可直接用 mqtt.publish(...)
static Preferences provNvs;

// 运行期配置 (开机从 NVS 读)
static String provSsid, provPass;
static String provMqttHost, provMqttUser, provMqttPass;
static int    provMqttPort = 8883;
static bool   provMqttWanted = false;
static unsigned long provLastMqttTry = 0;
static unsigned long provBootMs = 0;

// 业务钩子: 用户可选设置
static void (*provUserOnMqttConnect)() = nullptr;                 // MQTT 连上后回调(在这里 subscribe 你的主题)
static void (*provUserOnMqttMessage)(char*, byte*, unsigned) = nullptr; // 收到非 OTA 主题消息时回调

// 待办标志(BLE 回调里不做耗时操作, 丢给 loop)
static volatile bool provDoScan = false, provDoWifi = false, provDoMqtt = false;
static String provPendSsid, provPendPass;
static String provRxBuf;

// ---- OTA over BLE 状态 ----
static volatile bool   provOtaActive   = false;
static volatile size_t provOtaSize     = 0;
static volatile size_t provOtaRecv     = 0;
static volatile bool   provOtaAckPend  = false;
static volatile bool   provOtaErr      = false;
static String          provOtaErrMsg;
static volatile size_t provOtaNextAck  = 0;
#define PROV_OTA_WIN_BYTES 2160        // 每收到这么多字节给网页发一次 ack(流控窗口), 与网页一致(12*180)

static String provTopicPrefix = PROV_TOPIC_PREFIX;
static String provTopicStatus, provTopicOta;

// ======================================================================
// BLE 发送(分片, 行分隔)
// ======================================================================
static void provBleSend(const String& line) {
  if (!provTx || !provBleConnected) return;
  String s = line + "\n";
  const size_t CHUNK = 180;
  for (size_t i = 0; i < s.length(); i += CHUNK) {
    String part = s.substring(i, min(i + CHUNK, (size_t)s.length()));
    provTx->setValue((uint8_t*)part.c_str(), part.length());
    provTx->notify();
    delay(12);
  }
}
static void provLog(const String& m) {
  JsonDocument d; d["t"] = "log"; d["m"] = m;
  String o; serializeJson(d, o); provBleSend(o);
  Serial.println("[prov] " + m);
}
static void provSendStatus() {
  JsonDocument d;
  d["t"]    = "status";
  d["wifi"] = (WiFi.status() == WL_CONNECTED);
  d["ip"]   = WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["mqtt"] = mqtt.connected();
  d["heap"] = ESP.getMaxAllocHeap();
  String o; serializeJson(d, o); provBleSend(o);
}
static void provSendOta(const char* phase, const String& msg = "") {
  JsonDocument d;
  d["t"]     = "ota";
  d["phase"] = phase;
  if (provOtaSize) d["pct"] = (int)(provOtaRecv * 100 / provOtaSize);
  d["recv"]  = (uint32_t)provOtaRecv;
  if (msg.length()) d["msg"] = msg;
  String o; serializeJson(d, o); provBleSend(o);
}

// ======================================================================
// NVS 持久化 (你现有代码没存, 重启就丢配置 —— 这里补上)
// ======================================================================
static void provLoadCfg() {
  provNvs.begin("prov", true);
  provSsid     = provNvs.getString("ssid", "");
  provPass     = provNvs.getString("pass", "");
  provMqttHost = provNvs.getString("mhost", "");
  provMqttUser = provNvs.getString("muser", "");
  provMqttPass = provNvs.getString("mpass", "");
  provMqttPort = provNvs.getInt("mport", 8883);
  provNvs.end();
}
static void provSaveWifi(const String& s, const String& p) {
  provNvs.begin("prov", false);
  provNvs.putString("ssid", s);
  provNvs.putString("pass", p);
  provNvs.end();
}
static void provSaveMqtt() {
  provNvs.begin("prov", false);
  provNvs.putString("mhost", provMqttHost);
  provNvs.putString("muser", provMqttUser);
  provNvs.putString("mpass", provMqttPass);
  provNvs.putInt("mport", provMqttPort);
  provNvs.end();
}
static void provClearCfg() {
  provNvs.begin("prov", false);
  provNvs.clear();
  provNvs.end();
}

// ======================================================================
// 释放 BLE 以腾出连续堆给 TLS (沿用你验证过的做法)
// ======================================================================
static void provFreeBle() {
  if (provBleFreed) return;
  provBleFreed = true;
  provLog("释放 BLE 内存以完成 TLS 握手(蓝牙会断开, 之后走 MQTT/URL-OTA)");
  delay(300);
  provTx = nullptr;
  provBleConnected = false;
  BLEDevice::deinit(true);
  delay(100);
  Serial.printf("[prov] BLE 已释放, 最大连续块=%u\n", ESP.getMaxAllocHeap());
}

// ======================================================================
// MQTT
// ======================================================================
static void provMqttCb(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  // OTA 主题: 收到固件 URL -> 拉取升级
  if (t == provTopicOta) {
    String url; url.reserve(len);
    for (unsigned i = 0; i < len; i++) url += (char)payload[i];
    url.trim();
    if (url.length()) { extern void provUrlOta(const String&); provUrlOta(url); }
    return;
  }
  if (provUserOnMqttMessage) provUserOnMqttMessage(topic, payload, len);
}

static bool provMqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!provMqttHost.length()) { provLog("未配置 MQTT 地址"); return false; }

  provTopicStatus = provTopicPrefix + "/status";
  provTopicOta    = provTopicPrefix + "/ota";      // 往这个主题发布固件URL即可远程升级

  IPAddress ip;
  if (!WiFi.hostByName(provMqttHost.c_str(), ip)) {
    provLog("DNS 解析失败: " + provMqttHost);
    provBleSend("{\"t\":\"mqtt\",\"ok\":false,\"rc\":-2}");
    return false;
  }

  if (ESP.getMaxAllocHeap() < 50000) provFreeBle();   // 堆不够先释放 BLE

  provNet.setInsecure();
  provNet.setTimeout(15);
  provNet.setHandshakeTimeout(30);

  if (!provNet.connected() && !provNet.connect(provMqttHost.c_str(), provMqttPort)) {
    char err[128] = {0};
    int e = provNet.lastError(err, sizeof(err));
    provLog(String("TLS 失败 ") + e + " " + err);
    if (!provBleFreed) provFreeBle();
    provNet.stop();
    provBleSend("{\"t\":\"mqtt\",\"ok\":false,\"rc\":-2}");
    return false;
  }

  mqtt.setServer(provMqttHost.c_str(), provMqttPort);
  mqtt.setCallback(provMqttCb);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(15);

  String cid = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = mqtt.connect(cid.c_str(), provMqttUser.c_str(), provMqttPass.c_str(),
                         provTopicStatus.c_str(), 1, true, "offline");
  if (ok) {
    mqtt.publish(provTopicStatus.c_str(), "online", true);
    mqtt.subscribe(provTopicOta.c_str(), 1);        // 远程 OTA 通道
    if (provUserOnMqttConnect) provUserOnMqttConnect();
    provLog("MQTT 已连接");
  } else {
    provLog("MQTT 失败 rc=" + String(mqtt.state()));
    provNet.stop();
  }
  JsonDocument d; d["t"] = "mqtt"; d["ok"] = ok; d["rc"] = mqtt.state();
  String o; serializeJson(d, o); provBleSend(o);
  return ok;
}

// ======================================================================
// URL OTA (WiFi 拉取 .bin, 蓝牙已释放也能用; MQTT 也可远程触发)
// ======================================================================
static bool provUrlOtaProgressAdded = false;
void provUrlOta(const String& url) {
  if (WiFi.status() != WL_CONNECTED) { provLog("URL-OTA: WiFi 未连接"); return; }
  provLog("URL-OTA 开始: " + url);

  httpUpdate.rebootOnUpdate(false);
  if (!provUrlOtaProgressAdded) {
    provUrlOtaProgressAdded = true;
    httpUpdate.onProgress([](int cur, int total) {
      static int last = -1;
      int pct = total ? cur * 100 / total : 0;
      if (pct != last) { last = pct; Serial.printf("[prov] URL-OTA %d%%\n", pct); }
    });
  }

  t_httpUpdate_return r;
  if (url.startsWith("https")) {
    if (ESP.getMaxAllocHeap() < 50000) provFreeBle();   // https 需要连续堆
    WiFiClientSecure c; c.setInsecure();
    r = httpUpdate.update(c, url);
  } else {
    WiFiClient c;
    r = httpUpdate.update(c, url);
  }

  switch (r) {
    case HTTP_UPDATE_OK:
      provLog("URL-OTA 成功, 重启");
      if (mqtt.connected()) mqtt.publish(provTopicStatus.c_str(), "updated", true);
      delay(600); ESP.restart();
      break;
    case HTTP_UPDATE_FAILED:
      provLog(String("URL-OTA 失败: ") + httpUpdate.getLastErrorString());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      provLog("URL-OTA: 无更新");
      break;
  }
}

// ======================================================================
// WiFi
// ======================================================================
static bool provWifiConnect(const String& ssid, const String& pass, bool save) {
  provLog("连接 WiFi: " + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok && save) { provSsid = ssid; provPass = pass; provSaveWifi(ssid, pass); }
  return ok;
}

// ======================================================================
// 任务执行 (loop 里调用, 不在 BLE 回调里做耗时操作)
// ======================================================================
static void provTaskScan() {
  provDoScan = false;
  provLog("扫描 WiFi...");
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n && i < 30; i++) {
    JsonDocument d;
    d["t"]    = "net";
    d["ssid"] = WiFi.SSID(i);
    d["rssi"] = WiFi.RSSI(i);
    d["lock"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    String o; serializeJson(d, o); provBleSend(o);
  }
  WiFi.scanDelete();
  provBleSend("{\"t\":\"scan_done\"}");
}
static void provTaskWifi() {
  provDoWifi = false;
  bool ok = provWifiConnect(provPendSsid, provPendPass, true);
  JsonDocument d; d["t"] = "wifi"; d["ok"] = ok; d["ip"] = WiFi.localIP().toString();
  String o; serializeJson(d, o); provBleSend(o);
}
static void provTaskMqtt() {
  provDoMqtt = false;
  provMqttWanted = true;
  provMqttConnect();
}

// ======================================================================
// 命令处理 (网页 -> ESP32, JSON 行)
// ======================================================================
static void provHandleCommand(const String& line) {
  JsonDocument d;
  if (deserializeJson(d, line)) { provLog("JSON 解析失败"); return; }
  String cmd = d["cmd"] | "";

  if (cmd == "scan") {
    provDoScan = true;

  } else if (cmd == "wifi") {
    provPendSsid = String((const char*)(d["ssid"] | ""));
    provPendPass = String((const char*)(d["pass"] | ""));
    provDoWifi = true;

  } else if (cmd == "mqtt") {
    String h = String((const char*)(d["host"] | ""));
    String u = String((const char*)(d["user"] | ""));
    String p = String((const char*)(d["pass"] | ""));
    int    q = d["port"] | 0;
    if (h.length()) provMqttHost = h;
    if (u.length()) provMqttUser = u;
    if (p.length()) provMqttPass = p;
    if (q > 0)      provMqttPort = q;
    provSaveMqtt();                 // 记住, 下次开机自动用
    provDoMqtt = true;

  } else if (cmd == "status") {
    provSendStatus();

  } else if (cmd == "reset") {
    provClearCfg();
    provLog("已清除保存的 WiFi/MQTT 配置");

  } else if (cmd == "ota_url") {          // 网页给 URL, ESP 拉取升级
    String url = String((const char*)(d["url"] | ""));
    if (url.length()) provUrlOta(url);

  } else if (cmd == "ota_begin") {        // 开始 BLE 直传本地 .bin
    if (provBleFreed) { provSendOta("error", "BLE 已释放, 请改用 URL-OTA"); return; }
    size_t sz = (size_t)(uint32_t)(d["size"] | 0);
    if (sz == 0) { provSendOta("error", "size=0"); return; }
    if (Update.isRunning()) Update.abort();      // 中止上一次没走完的升级, 允许直接重试
    provOtaActive = false;
    if (!Update.begin(sz)) { provSendOta("error", Update.errorString()); return; }
    provOtaActive = true; provOtaSize = sz; provOtaRecv = 0;
    provOtaAckPend = false; provOtaErr = false;
    provOtaNextAck = PROV_OTA_WIN_BYTES;
    provLog("BLE-OTA 开始, 大小=" + String((uint32_t)sz));
    provSendOta("ready");

  } else if (cmd == "ota_end") {
    if (!provOtaActive) { provSendOta("error", "未在 OTA 中"); return; }
    provOtaActive = false;
    if (provOtaErr) { provSendOta("error", provOtaErrMsg); Update.abort(); return; }
    if (Update.end(true)) {
      provSendOta("done");
      delay(700);
      ESP.restart();
    } else {
      provSendOta("error", Update.errorString());
    }

  } else if (cmd == "ota_abort") {
    if (provOtaActive) { provOtaActive = false; Update.abort(); provLog("BLE-OTA 已取消"); }
  }
}

// ======================================================================
// BLE 回调
// ======================================================================
class ProvSrvCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { provBleConnected = true; Serial.println("[prov] BLE 已连接"); }
  void onDisconnect(BLEServer* s) override {
    provBleConnected = false;
    if (provBleFreed) return;
    BLEDevice::startAdvertising();
  }
};
class ProvRxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = String(c->getValue().c_str());
    for (size_t i = 0; i < v.length(); i++) {
      char ch = v[i];
      if (ch == '\n') {
        String line = provRxBuf; provRxBuf = ""; line.trim();
        if (line.length()) provHandleCommand(line);
      } else {
        provRxBuf += ch;
        if (provRxBuf.length() > 600) provRxBuf = "";
      }
    }
  }
};
class ProvOtaCB : public BLECharacteristicCallbacks {   // 固件原始字节
  void onWrite(BLECharacteristic* c) override {
    if (!provOtaActive) return;
    String v = c->getValue();
    size_t n = v.length();
    if (n == 0) return;
    size_t w = Update.write((uint8_t*)v.c_str(), n);
    if (w != n) { provOtaErr = true; provOtaErrMsg = Update.errorString(); provOtaActive = false; return; }
    provOtaRecv += n;
    // 到达窗口边界或收满 -> 通知 loop 给网页发 ack(流控)
    if (provOtaRecv >= provOtaSize || provOtaRecv >= provOtaNextAck) {
      provOtaNextAck += PROV_OTA_WIN_BYTES;
      provOtaAckPend = true;
    }
  }
};

// ======================================================================
// 对外 API
// ======================================================================
inline void provisionSetOnMqttConnect(void (*fn)())                      { provUserOnMqttConnect = fn; }
inline void provisionSetOnMqttMessage(void (*fn)(char*, byte*, unsigned)) { provUserOnMqttMessage = fn; }

inline void provisionSetup(const char* deviceName) {
  Serial.begin(115200);
  delay(200);
  provBootMs = millis();

  WiFi.mode(WIFI_STA);
  provLoadCfg();

  BLEDevice::init(deviceName);
  BLEDevice::setMTU(247);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ProvSrvCB());
  BLEService* svc = server->createService(PROV_SVC_UUID);

  provTx = svc->createCharacteristic(PROV_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  provTx->addDescriptor(new BLE2902());

  BLECharacteristic* rx = svc->createCharacteristic(
      PROV_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new ProvRxCB());

  BLECharacteristic* ota = svc->createCharacteristic(
      PROV_OTA_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  ota->setCallbacks(new ProvOtaCB());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(PROV_SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[prov] BLE 广播中: %s\n", deviceName);

  // 有保存的 WiFi 就自动连(蓝牙保持在线, 方便随时 BLE-OTA/重配)
  if (provSsid.length()) {
    if (provWifiConnect(provSsid, provPass, false))
      Serial.println("[prov] 已自动连回 WiFi: " + WiFi.localIP().toString());
  }
}

inline void provisionLoop() {
  if (provDoScan) provTaskScan();
  if (provDoWifi) provTaskWifi();
  if (provDoMqtt) provTaskMqtt();

  if (provOtaActive && provOtaAckPend) { provOtaAckPend = false; provSendOta("ack"); }
  if (!provOtaActive && provOtaErr)    { provOtaErr = false; provSendOta("error", provOtaErrMsg); }

  // 可选: 开机 N ms 内无人用 BLE 且已配好 MQTT -> 自动进运行态(释放BLE, 连MQTT)
  if (PROV_AUTO_MQTT_MS > 0 && !provMqttWanted && !provBleConnected &&
      provMqttHost.length() && WiFi.status() == WL_CONNECTED &&
      millis() - provBootMs > PROV_AUTO_MQTT_MS) {
    provLog("自动进入运行态");
    provMqttWanted = true;
    provMqttConnect();
  }

  if (provMqttWanted && WiFi.status() == WL_CONNECTED) {
    if (mqtt.connected()) mqtt.loop();
    else if (millis() - provLastMqttTry > 5000) { provLastMqttTry = millis(); provMqttConnect(); }
  }
}
