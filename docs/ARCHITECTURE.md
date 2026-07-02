# ESP32-S3 BLE Configuration + MQTT + OTA

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                        用户浏览器                            │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              Web Config Page (HTML)                  │   │
│  │  - Web Bluetooth API 扫描/连接 ESP32 BLE             │   │
│  │  - 扫描 WiFi 列表 → 用户选择 SSID + 输入密码          │   │
│  │  - 输入 MQTT 凭证 (broker URL, username, password)    │   │
│  │  - 输入 OTA HTTPS URL                                 │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                            │ BLE GATT
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                     ESP32-S3 DevKit                         │
│                                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │   BLE    │  │  WiFi    │  │  MQTT    │  │   OTA    │   │
│  │  GATT    │──│  Manager │──│  Client  │──│  Updater │   │
│  │  Service │  │          │  │ (TLS)    │  │          │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
│       │              │              │              │        │
│       └──────────────┴──────────────┴──────────────┘        │
│                          │                                  │
│                    ┌─────────────┐                          │
│                    │    NVS      │                          │
│                    │  (持久存储)  │                          │
│                    └─────────────┘                          │
└─────────────────────────────────────────────────────────────┘
                            │ MQTT over TLS (8883)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                      HiveMQ Cloud                           │
│  Broker: xxxxxxxx.s1.eu.hivemq.cloud:8883                  │
│                                                             │
│  Topics:                                                    │
│  ┌ esp32/{device_id}/config    (website → ESP32)           │
│  ┌ esp32/{device_id}/status    (ESP32 → website)           │
│  └ esp32/{device_id}/data      (bidirectional data)        │
└─────────────────────────────────────────────────────────────┘
```

## BLE GATT Service UUID

```
Service:              12345678-1234-1234-1234-123456789abc
  ├─ Char WiFi Scan:  12345678-1234-1234-1234-123456789ab1 (READ, NOTIFY)
  ├─ Char WiFi Config:12345678-1234-1234-1234-123456789ab2 (WRITE)
  ├─ Char MQTT Config:12345678-1234-1234-1234-123456789ab3 (WRITE)
  ├─ Char OTA URL:    12345678-1234-1234-1234-123456789ab4 (WRITE)
  ├─ Char Control:    12345678-1234-1234-1234-123456789ab5 (WRITE)
  └─ Char Status:     12345678-1234-1234-1234-123456789ab6 (READ, NOTIFY)
```

## BLE 数据包格式

### WiFi Scan Results (ESP32 → Browser)
```
[COUNT:1byte][SSID1_LEN:1byte][SSID1...][RSSI1:1byte][AUTH1:1byte]...
```

### WiFi Config (Browser → ESP32)
```
SSID_LEN(1) + SSID + PASSWORD_LEN(1) + PASSWORD
```

### MQTT Config (Browser → ESP32)
```
BROKER_LEN(1) + BROKER + PORT(2) + USER_LEN(1) + USER + PASS_LEN(1) + PASS
```

### Control Commands (Browser → ESP32)
```
CMD_SCAN_WIFI    = 0x01
CMD_CONNECT_WIFI = 0x02
CMD_CONNECT_MQTT = 0x03
CMD_START_OTA    = 0x04
CMD_SAVE_CONFIG  = 0x05
CMD_RESET_CONFIG = 0x06
```

### Status (ESP32 → Browser, NOTIFY)
```
STATUS_OK           = 0x00
STATUS_SCANNING     = 0x01
STATUS_WIFI_CONNECTING = 0x02
STATUS_WIFI_CONNECTED   = 0x03
STATUS_WIFI_FAILED      = 0x04
STATUS_MQTT_CONNECTING = 0x05
STATUS_MQTT_CONNECTED   = 0x06
STATUS_MQTT_FAILED       = 0x07
STATUS_OTA_STARTING      = 0x08
STATUS_OTA_PROGRESS      = 0x09
STATUS_OTA_SUCCESS       = 0x0A
STATUS_OTA_FAILED        = 0x0B
STATUS_ERROR        = 0xFF
```

## MQTT Topic 结构

```
esp32/<DEVICE_ID>/config   — 网站发命令给 ESP32
esp32/<DEVICE_ID>/status   — ESP32 回复状态
esp32/<DEVICE_ID>/data     — 双向数据通道
```

## 依赖库 (Arduino IDE)

- `WiFi` (内置)
- `BLE` (ESP32 内置)
- `PubSubClient` (安装)
- `Preferences` (ESP32 内置)
- `Update` (ESP32 内置)
- `esp_timer.h` (ESP32 内置)

## Web 页面功能流程

1. 打开页面 → 点击"连接设备" → 浏览器扫描 BLE 设备
2. 选择 `ESP32_Config` 设备 → 连接 GATT Server
3. 点击"扫描 WiFi" → ESP32 扫描 → 返回 SSID 列表 → 用户选择
4. 输入 WiFi 密码 → 点击"保存 WiFi"
5. 输入 MQTT Broker URL、用户名、密码 → 点击"保存 MQTT"
6. 输入 OTA HTTPS URL → 点击"开始 OTA"
7. 页面显示 ESP32 升级进度
