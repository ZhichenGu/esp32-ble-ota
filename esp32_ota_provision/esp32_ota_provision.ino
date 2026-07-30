/*
 * esp32_ota_provision.ino  —  模板主程序
 * ============================================================
 *  这是"最小骨架": 只有配网 + OTA, 没有具体业务。
 *  以后做新项目, 直接复制本目录(带 provision.h / partitions.csv),
 *  把你的逻辑写进下面标注的【业务区】即可。
 *
 *  编译/烧录设置(Arduino IDE 工具菜单, 或用 README 里的 arduino-cli 命令):
 *    开发板 : ESP32S3 Dev Module
 *    Flash Size      : 16MB (128Mb)
 *    PSRAM           : OPI PSRAM      (N16R8 是 8MB OPI PSRAM)
 *    Partition Scheme: Custom         (会自动读本目录 partitions.csv, 双 OTA 槽)
 *    USB CDC On Boot : Enabled        (串口调试用)
 * ============================================================
 */

// 想让设备开机 60 秒没人连蓝牙就自动进运行态(连 MQTT)? 取消下一行注释:
// #define PROV_AUTO_MQTT_MS 60000

#include "provision.h"

// ---------------- 业务区: MQTT 连上后订阅你的主题 ----------------
void onMyMqttConnect() {
  // 例: mqtt.subscribe("esp32/led/set", 1);
}

// ---------------- 业务区: 收到 MQTT 消息 ----------------
void onMyMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg; msg.reserve(len);
  for (unsigned i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("MQTT <- %s : %s\n", topic, msg.c_str());
  // 例: 解析 msg, 驱动你的外设
}

void setup() {
  provisionSetup("ESP32-OTA");                 // BLE 广播名, 网页按这个找设备
  provisionSetOnMqttConnect(onMyMqttConnect);
  provisionSetOnMqttMessage(onMyMqttMessage);

  // ---------------- 业务区: 你的硬件初始化 ----------------
  // pinMode(2, OUTPUT);
}

void loop() {
  provisionLoop();                             // 必须每圈调用: 处理 BLE/WiFi/MQTT/OTA

  // ---------------- 业务区: 你的主循环 ----------------
  // digitalWrite(2, ...);
  delay(5);
}
